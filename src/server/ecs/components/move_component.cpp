// move_component.cpp — 路径积分移动（JPH::Vec3 / 无界 GridKey）

#include "ecs/components/move_component.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "ecs/entity/entity.h"
#include "ecs/systems/map_grid.h"
#include "ecs/systems/map_system.h"
#include "ecs/systems/world_system.h"

uint64_t g_pass_id_3d = 0;

namespace {

constexpr float kCheckObstacleDistance = 2.f * 6.f;
constexpr float kOffsetSize = 0.1f;
constexpr float kEps = 1e-6f;

float LengthSq(const Vector3D& v) { return v.LengthSq(); }
float Length(const Vector3D& v) { return v.Length(); }

bool IsZeroVec(const Vector3D& v) { return LengthSq(v) <= kEps * kEps; }

Vector3D Normalized(const Vector3D& v) {
  const float len = Length(v);
  if (len <= kEps) {
    return Vector3D(0.f, 0.f, 0.f);
  }
  return Vector3D(v / len);
}

Vector3D MapEntityCenter(MapSystem* map, const EntityPtr& ent) {
  Vector3D c = ent->GetPosition();
  map->MapIndexToCenterPos(c.GridX(), c.GridY(), c.GridZ(), c);
  return c;
}

struct SortEntityData3D {
  EntityPtr entity;
  Vector3D cvt;
};

struct SortRelationEntityFunc3D {
  bool operator()(const SortEntityData3D& a, const SortEntityData3D& b) const {
    return LengthSq(a.cvt) < LengthSq(b.cvt);
  }
};

}  // namespace

void MoveComponent::Clear() {
  move_status_ = kMoveStatusNone;
  target_position_.Set(0.f, 0.f, 0.f);
  cur_region_paths_.clear();
  move_arc_paths_.clear();
  pre_check_obstacle_pos_.Set(-1.f, -1.f, -1.f);
  pre_check_obstacle_valid_ = false;
}

bool MoveComponent::MoveTo(const Vector3D& pos, MoveStopCallback cb) {
  Clear();
  target_position_ = pos;
  move_call_back_ = std::move(cb);
  move_status_ = kMoveStatusPathFind;
  // 真实寻路接入前：直线终点；DoFindPath 在 Tick 首次需要 entity，这里先缓存终点
  // 实际 Init 在 TickFrame 里用当前位置调用 DoFindPath（见 TickFrame）
  return true;
}

void MoveComponent::OnStopMove(bool is_done, MoveStopReason reason) {
  // entity 由 TickFrame/Cancel 路径保证：若仅 Cancel，调用方先保证脏标记可选
  MoveStopCallback cb;
  cb.swap(move_call_back_);
  Clear();
  if (cb) {
    cb(is_done, reason);
  }
}

bool MoveComponent::IsEmptyMoveTarget() const {
  return cur_region_paths_.empty() && move_arc_paths_.empty();
}

Vector3D* MoveComponent::GetPathMoveTarget() {
  if (cur_region_paths_.empty()) {
    return nullptr;
  }
  auto& state_path = cur_region_paths_.front();
  if (state_path.paths.empty()) {
    return nullptr;
  }
  return &state_path.paths.front();
}

Vector3D* MoveComponent::GetMoveTarget() {
  if (IsEmptyMoveTarget()) {
    return nullptr;
  }
  if (!move_arc_paths_.empty()) {
    return &move_arc_paths_.front();
  }
  return GetPathMoveTarget();
}

bool MoveComponent::PopCurMoveTarget() {
  if (cur_region_paths_.empty()) {
    return true;
  }
  auto& state_path = cur_region_paths_.front();
  if (state_path.paths.empty()) {
    cur_region_paths_.pop_front();
    return true;
  }
  state_path.paths.pop_front();
  if (!state_path.paths.empty()) {
    return true;
  }
  cur_region_paths_.pop_front();
  pre_check_obstacle_pos_.Set(0.f, 0.f, 0.f);
  pre_check_obstacle_valid_ = false;
  return true;
}

bool MoveComponent::PopMoveTarget() {
  if (!move_arc_paths_.empty()) {
    move_arc_paths_.pop_front();
    if (move_arc_paths_.empty() && move_status_ == kMoveStatusMoveArc) {
      move_status_ = kMoveStatusMoving;
    }
    return true;
  }
  return PopCurMoveTarget();
}

bool MoveComponent::UpdateMovePos(WorldSystem* world, Entity* entity,
                                  float dt_sec) {
  if (!world || !entity) {
    return false;
  }
  const float speed = GetCurSpeed();
  if (speed <= 0.f) {
    entity->SetPropertyDirty(EntityPropertyType::kStopMove);
    OnStopMove(false, MoveStopReason::kZeroSpeed);
    return false;
  }

  Vector3D cur_pos = entity->GetPosition();
  Vector3D direction = facing_dir_;
  bool is_update = false;
  bool is_stop = false;
  float time_left = dt_sec;

  while (time_left > 0.f && !IsEmptyMoveTarget()) {
    Vector3D* target_ptr = GetMoveTarget();
    if (target_ptr == nullptr) {
      entity->SetPropertyDirty(EntityPropertyType::kStopMove);
      OnStopMove(false, MoveStopReason::kUnknown);
      is_stop = true;
      break;
    }

    const Vector3D target_pos = *target_ptr;
    is_update = true;
    Vector3D v = target_pos - cur_pos;
    if (!IsZeroVec(v)) {
      direction = v;
    }

    const float dist = Length(v);
    if (dist <= kEps) {
      cur_pos = target_pos;
      if (!PopMoveTarget()) {
        entity->SetPropertyDirty(EntityPropertyType::kStopMove);
        OnStopMove(false, MoveStopReason::kUnknown);
        is_stop = true;
        break;
      }
      continue;
    }

    const float seg_time = dist / speed;
    if (seg_time <= time_left) {
      time_left -= seg_time;
      cur_pos = target_pos;
      if (!PopMoveTarget()) {
        entity->SetPropertyDirty(EntityPropertyType::kStopMove);
        OnStopMove(false, MoveStopReason::kUnknown);
        is_stop = true;
        break;
      }
      continue;
    }

    const float step = speed * time_left;
    cur_pos = cur_pos + Normalized(v) * step;
    time_left = 0.f;
    break;
  }

    if (is_update) {
    facing_dir_ = direction;
    entity->SetVelocity(Normalized(direction) * speed);
    world->MoveEntity(entity->SharedHandle(), cur_pos);
  }

  if (!is_stop && IsEmptyMoveTarget()) {
    entity->SetVelocity(JPH::Vec3::sZero());
    entity->SetPropertyDirty(EntityPropertyType::kStopMove);
    OnStopMove(true, MoveStopReason::kSuccess);
  }
  return false;
}

void MoveComponent::CheckPreObstacle(WorldSystem* world, Entity* entity) {
  if (!world || !entity) {
    return;
  }
  // 绕弧进行中不要清空 move_arc_paths_，否则每帧重建会导致永不结束
  if (move_status_ == kMoveStatusMoveArc && !move_arc_paths_.empty()) {
    return;
  }
  if (move_status_ != kMoveStatusMoving || cur_region_paths_.empty() ||
      !move_arc_paths_.empty()) {
    return;
  }

  MapSystem& map = world->Map();
  const Vector3D cur_pos = entity->GetPosition();
  Vector3D* target_pos_ptr = GetPathMoveTarget();
  if (target_pos_ptr == nullptr) {
    return;
  }

  Vector3D target_pos = *target_pos_ptr;
  const Vector3D v = target_pos - cur_pos;
  Vector3D check_pos = target_pos;
  if (LengthSq(v) > kCheckObstacleDistance * kCheckObstacleDistance) {
    check_pos = cur_pos + Normalized(v) * kCheckObstacleDistance;
  }

  if (pre_check_obstacle_valid_) {
    const Vector3D dest = pre_check_obstacle_pos_ - check_pos;
    if (LengthSq(dest) <= kCheckObstacleDistance * kCheckObstacleDistance) {
      return;
    }
  }

  pre_check_obstacle_pos_ = check_pos;
  pre_check_obstacle_valid_ = true;

  std::list<GridKey> grid_keys;
  map.LineInterGrid(cur_pos, check_pos, grid_keys);

  std::set<uint64_t> checked_entities;
  std::list<SortEntityData3D> relation_entities;
  for (const GridKey& key : grid_keys) {
    MapGrid* grid = map.GetGrid(key.x, key.y, key.z);
    if (grid == nullptr) {
      continue;
    }
    for (const auto& [id, ent] : grid->GetEntities()) {
      (void)id;
      if (!ent || ent->GetEntityType() != EntityType::kTown) {
        continue;
      }
      if (checked_entities.count(ent->GetId()) > 0) {
        continue;
      }
      const float cl = static_cast<float>(ent->GetCollision());
      if (cl <= 0.f) {
        continue;
      }

      Vector3D cvt;
      if (!map.IsRelationEntity(cur_pos, target_pos, MapEntityCenter(&map, ent),
                                cl - kOffsetSize, cvt)) {
        continue;
      }

      checked_entities.insert(ent->GetId());
      relation_entities.push_back({ent, cvt});
    }
  }

  if (relation_entities.empty()) {
    return;
  }

  relation_entities.sort(SortRelationEntityFunc3D());
  Vector3D start_pos = cur_pos;
  move_arc_paths_.clear();
  std::list<Vector3D> arc_paths;
  for (auto& item : relation_entities) {
    float cl = static_cast<float>(item.entity->GetCollision() +
                                  entity->GetCollision());
    const float scale = static_cast<float>(item.entity->GetScale());
    if (scale > 0.f && cl > scale) {
      cl = scale;
    }

    map.CalcMoveSpherePath(start_pos, target_pos,
                           MapEntityCenter(&map, item.entity), cl, arc_paths);
    for (const auto& path : arc_paths) {
      move_arc_paths_.push_back(path);
    }
    if (!arc_paths.empty()) {
      start_pos = arc_paths.back();
    }
    arc_paths.clear();
  }

  if (!move_arc_paths_.empty()) {
    move_status_ = kMoveStatusMoveArc;
  }
}

void MoveComponent::HandleMove(WorldSystem* world, Entity* entity,
                               float dt_sec) {
  CheckPreObstacle(world, entity);
  if (move_status_ == kMoveStatusMoving ||
      move_status_ == kMoveStatusMoveArc) {
    UpdateMovePos(world, entity, dt_sec);
  }
}

void MoveComponent::StartMove(Entity* entity) {
  if (entity) {
    entity->SetPropertyDirty(EntityPropertyType::kMove);
  }
  move_status_ = kMoveStatusMoving;
}

void MoveComponent::InitPathFindData(const std::list<Vector3D>& find_path_list) {
  cur_region_paths_.clear();
  StatePathData3D path_data;
  path_data.pass_id = ++g_pass_id_3d;
  path_data.paths = find_path_list;
  cur_region_paths_.push_back(std::move(path_data));

  if (!cur_region_paths_.empty() && !cur_region_paths_.front().paths.empty()) {
    target_position_ = cur_region_paths_.front().paths.front();
  }
}

void MoveComponent::OnPathFindResult(Entity* entity,
                                     const std::list<Vector3D>& find_path_list) {
  InitPathFindData(find_path_list);
  if (move_status_ == kMoveStatusPathFind) {
    StartMove(entity);
  }
}

void MoveComponent::DoFindPath(Entity* entity, const Vector3D& /*start*/,
                               const Vector3D& end) {
  std::list<Vector3D> find_path_list;
  find_path_list.push_back(end);
  OnPathFindResult(entity, find_path_list);
}

void MoveComponent::TickFrame(WorldSystem* world, Entity* entity, float dt_sec) {
  if (move_status_ == kMoveStatusNone || !world || !entity) {
    return;
  }
  // PathFind：用当前位置生成直线路径后进入 Moving
  if (move_status_ == kMoveStatusPathFind && IsEmptyMoveTarget()) {
    DoFindPath(entity, entity->GetPosition(), target_position_);
  }
  HandleMove(world, entity, dt_sec);
}
