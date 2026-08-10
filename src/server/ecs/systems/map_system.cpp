// 无界稀疏地图实现。

#include "ecs/systems/map_system.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <vector>

#include "ecs/entity/entity.h"
#include "ecs/systems/map_grid.h"

MapSystem::MapSystem(SceneRegionType system_type)
    : system_type_(system_type) {}

MapSystem::~MapSystem() = default;

void MapSystem::SetEnterMapCallback(EnterMapCallback cb) {
    enter_map_cb_ = std::move(cb);
}

void MapSystem::SetLeaveMapCallback(LeaveMapCallback cb) {
    leave_map_cb_ = std::move(cb);
}

void MapSystem::SetMoveCallback(MoveCallback cb) {
    move_cb_ = std::move(cb);
}

void MapSystem::SetCrossGridCallback(CrossGridCallback cb) {
    cross_grid_cb_ = std::move(cb);
}

void MapSystem::NotifyEnterMap(const EntityPtr& entity) {
    if (enter_map_cb_ && entity) {
        enter_map_cb_(entity);
    }
}

void MapSystem::NotifyLeaveMap(const EntityPtr& entity) {
    if (leave_map_cb_ && entity) {
        leave_map_cb_(entity);
    }
}

void MapSystem::NotifyMove(const EntityPtr& entity, const Vector3D& old_pos,
                                const Vector3D& new_pos) {
    if (move_cb_ && entity) {
        move_cb_(entity, old_pos, new_pos);
    }
}

void MapSystem::NotifyCrossGrid(const EntityPtr& entity, int32_t old_gx,
                                     int32_t old_gy, int32_t old_gz, int32_t new_gx,
                                     int32_t new_gy, int32_t new_gz) {
    if (cross_grid_cb_ && entity) {
        cross_grid_cb_(entity, old_gx, old_gy, old_gz, new_gx, new_gy, new_gz);
    }
}

void MapSystem::Init() {
    hash_grids_.clear();
}

MapGrid* MapSystem::FindGrid(const GridKey& key) {
    return const_cast<MapGrid*>(
        static_cast<const MapSystem*>(this)->FindGrid(key));
}

const MapGrid* MapSystem::FindGrid(const GridKey& key) const {
    const auto it = hash_grids_.find(key);
    return it != hash_grids_.end() ? &it->second : nullptr;
}

MapGrid* MapSystem::EnsureGrid(int32_t x, int32_t y, int32_t z) {
    const GridKey key{x, y, z};
    auto it = hash_grids_.find(key);
    if (it == hash_grids_.end()) {
        it = hash_grids_
                 .emplace(key, MapGrid(key, static_cast<uint32_t>(MapCellStatus::kWalkable)))
                 .first;
    }
    return &it->second;
}

void MapSystem::ReleaseHashGridIfEmpty(const GridKey& key) {
    const auto it = hash_grids_.find(key);
    if (it != hash_grids_.end() && it->second.GetEntities().empty()) {
        hash_grids_.erase(it);
    }
}

size_t MapSystem::AllocatedGridCount() const {
    return hash_grids_.size();
}

bool MapSystem::MapIndexToCenterPos(int32_t x, int32_t y, int32_t z,
                                          Vector3D& pos) const {
    pos.Set(MapGridIndexToCenterWorld(x), MapGridIndexToCenterWorld(y),
            MapGridIndexToCenterWorld(z));
    return true;
}

MapGrid* MapSystem::GetGrid(int32_t x, int32_t y, int32_t z) {
    const GridKey key{x, y, z};
    return FindGrid(key);
}

const MapGrid* MapSystem::GetGrid(int32_t x, int32_t y, int32_t z) const {
    const GridKey key{x, y, z};
    return FindGrid(key);
}

MapGrid* MapSystem::GetGrid(const Vector3D& pos) {
    return GetGrid(pos.GridX(), pos.GridY(), pos.GridZ());
}

void MapSystem::CollectEntitiesInGridRect(
    int32_t gx_begin, int32_t gy_begin, int32_t gx_end_excl,
    int32_t gy_end_excl, const std::function<void(const EntityPtr&)>& fn) const {
    CollectEntitiesInGridBox(gx_begin, gy_begin, 0, gx_end_excl, gy_end_excl,
                             std::numeric_limits<int32_t>::max(), fn);
}

void MapSystem::CollectEntitiesInGridBox(
    int32_t gx_begin, int32_t gy_begin, int32_t gz_begin,
    int32_t gx_end_excl, int32_t gy_end_excl, int32_t gz_end_excl,
    const std::function<void(const EntityPtr&)>& fn) const {
    if (!fn) {
        return;
    }
    const auto in_range = [](int32_t v, int32_t lo, int32_t hi_excl) {
        if (hi_excl == std::numeric_limits<int32_t>::max()) return v >= lo;
        return v >= lo && v < hi_excl;
    };
    // 两阶段收集，避免回调修改 hash_grids_ 导致迭代器失效
    std::vector<EntityPtr> collected;
    std::unordered_set<uint64_t> seen;
    for (const auto& kv : hash_grids_) {
        const GridKey& key = kv.first;
        if (!in_range(key.x, gx_begin, gx_end_excl)) continue;
        if (!in_range(key.y, gy_begin, gy_end_excl)) continue;
        if (!in_range(key.z, gz_begin, gz_end_excl)) continue;
        for (const auto& [id, entity] : kv.second.GetEntities()) {
            (void)id;
            if (!entity) {
                continue;
            }
            if (seen.insert(entity->GetId()).second) {
                collected.push_back(entity);
            }
        }
    }
    for (const EntityPtr& entity : collected) {
        fn(entity);
    }
}

void MapSystem::OnEntityIntoMap(EntityPtr entity) {
    if (!entity) {
        return;
    }
    const Vector3D epos = entity->GetPosition();
    if (MapGrid* grid = EnsureGrid(epos.GridX(), epos.GridY(), epos.GridZ())) {
        grid->AddEntity(entity);
    }
}

void MapSystem::OnEntityLeaveMap(EntityPtr entity) {
    if (!entity) {
        return;
    }
    const Vector3D epos = entity->GetPosition();
    const GridKey key{epos.GridX(), epos.GridY(), epos.GridZ()};
    if (MapGrid* grid = FindGrid(key)) {
        grid->RemoveEntity(entity->GetId());
    }
    ReleaseHashGridIfEmpty(key);
}

void MapSystem::OnEntityChangePos(EntityPtr entity, const Vector3D& old_pos,
                                       const Vector3D& new_pos) {
    if (!entity) {
        return;
    }
    const auto clear_footprint = [&](const Vector3D& pos) {
        const GridKey key{pos.GridX(), pos.GridY(), pos.GridZ()};
        if (MapGrid* grid = FindGrid(key)) {
            grid->RemoveEntity(entity->GetId());
        }
        ReleaseHashGridIfEmpty(key);
    };
    const auto add_footprint = [&](const Vector3D& pos) {
        if (MapGrid* grid = EnsureGrid(pos.GridX(), pos.GridY(), pos.GridZ())) {
            grid->AddEntity(entity);
        }
    };
    clear_footprint(old_pos);
    add_footprint(new_pos);
}

bool MapSystem::LineInterGrid(const Vector3D& pos_0, const Vector3D& pos_1,
                                    std::list<GridKey>& grid_keys) {
    int x0 = pos_0.GridX();
    int y0 = pos_0.GridY();
    int z0 = pos_0.GridZ();
    int x1 = pos_1.GridX();
    int y1 = pos_1.GridY();
    int z1 = pos_1.GridZ();

    const int dx = std::abs(x1 - x0);
    const int dy = std::abs(y1 - y0);
    const int dz = std::abs(z1 - z0);
    const int sx = x0 < x1 ? 1 : -1;
    const int sy = y0 < y1 ? 1 : -1;
    const int sz = z0 < z1 ? 1 : -1;

    int dm = std::max({dx, dy, dz});
    if (dm == 0) {
        grid_keys.push_back(GridKey{static_cast<int32_t>(x0),
                                    static_cast<int32_t>(y0),
                                    static_cast<int32_t>(z0)});
        return true;
    }

    int err_x = dm / 2;
    int err_y = dm / 2;
    int err_z = dm / 2;
    int x = x0;
    int y = y0;
    int z = z0;

    for (int i = 0; i <= dm; ++i) {
        grid_keys.push_back(GridKey{static_cast<int32_t>(x),
                                    static_cast<int32_t>(y),
                                    static_cast<int32_t>(z)});
        err_x -= dx;
        if (err_x < 0) {
            err_x += dm;
            x += sx;
        }
        err_y -= dy;
        if (err_y < 0) {
            err_y += dm;
            y += sy;
        }
        err_z -= dz;
        if (err_z < 0) {
            err_z += dm;
            z += sz;
        }
    }
    return true;
}

namespace {
constexpr double kPi = 3.14159265358979323846;

float LengthSq(const Vector3D& v) { return v.LengthSq(); }
float LengthOf(const Vector3D& v) { return v.Length(); }
bool NearZero(const Vector3D& v) { return LengthSq(v) <= 1e-12f; }

Vector3D Normalized(const Vector3D& v) {
    const float len = LengthOf(v);
    if (len <= 1e-6f) {
        return Vector3D(0.f, 0.f, 0.f);
    }
    return Vector3D(v / len);
}

Vector3D RotateAroundAxis(const Vector3D& v, const Vector3D& axis,
                          float angle_rad) {
    const Vector3D n = Normalized(axis);
    if (NearZero(n)) {
        return v;
    }
    return Vector3D(JPH::Quat::sRotation(n, angle_rad) * v);
}
}  // namespace

bool MapSystem::IsRelationEntity(const Vector3D& start_pos,
                                 const Vector3D& target_pos,
                                 const Vector3D& center_pos, float radius,
                                 Vector3D& cvt) const {
    const Vector3D v = target_pos - start_pos;
    if (NearZero(v)) {
        return false;
    }

    const Vector3D cv = center_pos - start_pos;
    const float dir = v.Dot(cv);
    if (dir < 0.f) {
        return false;
    }

    const float r2 = radius * radius;
    const Vector3D tv = center_pos - target_pos;
    if (LengthSq(cv) <= r2 || LengthSq(tv) <= r2) {
        return false;
    }

    const Vector3D vt = cv - v * (v.Dot(cv) / LengthSq(v));
    if (LengthSq(vt) >= r2) {
        return false;
    }

    const float back_dir = (-cv).Dot(-tv);
    if (back_dir > 0.f) {
        return false;
    }

    cvt = cv - vt;
    return true;
}

PathCalcResult MapSystem::CalcMoveSpherePath(const Vector3D& start_pos,
                                             const Vector3D& target_pos,
                                             const Vector3D& center_pos,
                                             float radius,
                                             std::list<Vector3D>& paths) {
    if (radius <= 0.f) {
        return PathCalcResult::kNone;
    }

    const Vector3D v = target_pos - start_pos;
    if (NearZero(v)) {
        return PathCalcResult::kNone;
    }

    const Vector3D cv = center_pos - start_pos;
    const Vector3D tv = target_pos - center_pos;
    const float cvl = LengthOf(cv);
    const float tvl = LengthOf(tv);
    if (tvl <= 1e-6f) {
        return PathCalcResult::kNone;
    }

    const float r2 = radius * radius;
    if (cvl <= radius && tvl <= radius) {
        return PathCalcResult::kNone;
    }
    if (tvl <= radius) {
        const Vector3D temp = center_pos - Normalized(cv) * radius;
        paths.push_back(start_pos);
        paths.push_back(temp);
        return PathCalcResult::kPopTarget;
    }

    const float cosv = radius / tvl;
    const float lq = std::sqrt(std::max(0.f, tvl * tvl - r2));
    (void)lq;
    const float angle = std::acos(std::max(-1.f, std::min(1.f, cosv)));

    Vector3D axis = cv.Cross(tv);
    if (NearZero(axis)) {
        axis = Vector3D(0.f, 0.f, 1.f);
        if (NearZero(cv.Cross(axis))) {
            axis = Vector3D(0.f, 1.f, 0.f);
        }
    }
    axis = Normalized(axis);

    Vector3D tangent = Normalized(RotateAroundAxis(tv, axis, angle)) * radius;
    const Vector3D proj = -(cv - v * (v.Dot(cv) / LengthSq(v)));
    if (tangent.Dot(proj) < 0.f) {
        tangent = Normalized(RotateAroundAxis(tv, axis, -angle)) * radius;
    }
    Vector3D inter_pos_1 = center_pos - Normalized(cv) * radius;
    Vector3D inter_pos_2 = center_pos + tangent;

    MakeArcPath(inter_pos_1, inter_pos_2, center_pos, paths);
    paths.push_front(inter_pos_1);
    paths.push_back(inter_pos_2);
    return PathCalcResult::kMoveToTarget;
}

void MapSystem::MakeArcPath(const Vector3D& pos_1, const Vector3D& pos_2,
                            const Vector3D& center_pos,
                            std::list<Vector3D>& paths) {
    const Vector3D v1 = center_pos - pos_1;
    const Vector3D v2 = center_pos - pos_2;
    const float l1 = LengthOf(v1);
    const float l2 = LengthOf(v2);
    if (l1 <= 1e-6f || l2 <= 1e-6f) {
        return;
    }

    const float dot = v2.Dot(v1) / (l2 * l1);
    const double arv =
        std::acos(std::max(-1.0, std::min(1.0, static_cast<double>(dot)))) *
        180.0 / kPi;
    const int32_t add_count = static_cast<int32_t>(arv / 20.0);
    if (add_count <= 1) {
        return;
    }

    Vector3D axis = v1.Cross(v2);
    if (NearZero(axis)) {
        return;
    }
    axis = Normalized(axis);

    float step = static_cast<float>(20.0 / 180.0 * kPi);
    if (v1.Cross(pos_2 - pos_1).Dot(axis) < 0.f) {
        step = -step;
    }

    Vector3D temp = v1;
    for (int32_t i = 0; i < add_count; ++i) {
        temp = RotateAroundAxis(temp, axis, step);
        paths.push_back(center_pos - temp);
    }
}
