// 地图/世界测试基建：TestEntity、MakeWorld、GridCenter、足迹查询。
#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "ecs/entity/entity.h"
#include "ecs/components/move_component.h"
#include "ecs/components/transform_component.h"
#include "common/aoi_def.h"
#include "ecs/systems/map_grid.h"
#include "ecs/systems/map_system.h"
#include "ecs/systems/world_system.h"

namespace test {

// 测试用实体：覆写 IsPlayer（kPlayer 类型返回 true），构造时自动添加 MoveComponent。
// SerializeAppear/SerializeDirty 给空实现（测试只验 AOI 视野事件，不发真实包）。
class TestEntity : public Entity {
 public:
    TestEntity(WorldSystem* world, uint64_t id, EntityType type,
               const EntitySpawn& spawn)
        : Entity(world, id, type, spawn) {
        AddComponent<MoveComponent>().SetSpeed(100.0f);
    }

    bool IsPlayer() const override { return GetEntityType() == EntityType::kPlayer; }

    bool SerializeAppear(SerializeMsg&, const EntityPtr&) override {
        return false;
    }
    bool SerializeDirty(std::vector<SerializeMsg>&) override {
        return false;
    }
};

// 逻辑格 (gx,gy,gz) 中心点的世界坐标（用于 SpawnOnMap 定位）。
inline Vector3D GridCenter(uint32_t gx, uint32_t gy, uint32_t gz = 0) {
    return Vector3D(MapGridIndexToCenterWorld(static_cast<int32_t>(gx)),
                    MapGridIndexToCenterWorld(static_cast<int32_t>(gy)),
                    MapGridIndexToCenterWorld(static_cast<int32_t>(gz)));
}

// 默认实体工厂（兼容源测试调用 MakeDefaultEntityFactory()）
// 创建 TestEntity（带 MoveComponent + IsPlayer 覆写）
inline EntityFactory MakeDefaultEntityFactory() {
    return [](WorldSystem* w, uint64_t id, EntityType type,
              const EntitySpawn& spawn) -> EntityPtr {
        return std::make_shared<TestEntity>(w, id, type, spawn);
    };
}

// 创建并 Init 一个 WorldSystem（无界地图，w/h/d 参数忽略，保留兼容源测试调用）。
// 注入默认 Entity 工厂。
inline std::shared_ptr<WorldSystem> MakeWorld(
    uint32_t /*w*/ = 0, uint32_t /*h*/ = 0, uint32_t /*d*/ = 0,
    MapGridStorage /*storage*/ = MapGridStorage::kHash) {
    auto world = WorldSystem::Create(SceneRegionType::kMap);
    world->SetEntityFactory(MakeDefaultEntityFactory());
    world->Init();
    return world;
}

// 指定格实体列表中是否包含 entity id。
inline bool GridContainsEntity(MapSystem& map, int32_t gx, int32_t gy,
                               uint64_t id, int32_t gz = 0) {
    MapGrid* grid = map.GetGrid(gx, gy, gz);
    if (grid == nullptr) {
        return false;
    }
    for (const auto& [eid, e] : grid->GetEntities()) {
        (void)eid;
        if (e && e->GetId() == id) {
            return true;
        }
    }
    return false;
}

// 以 pos 所在逻辑格是否包含该实体。
inline bool FootprintContainsEntity(MapSystem& map, const Vector3D& pos,
                                    uint64_t id) {
    const int32_t x = pos.GridX();
    const int32_t y = pos.GridY();
    const int32_t z = pos.GridZ();
    return GridContainsEntity(map, x, y, id, z);
}

// 统计 tracked 实体在「自身所在格」中的引用次数（单格足迹每实体应为 0 或 1）。
// 注意：不可对格内全体实体累加——同格 N 人时会把每人误计为 N 次。
inline void CountEntityRefsOnMap(MapSystem& map,
                                 const std::vector<EntityPtr>& tracked,
                                 std::unordered_map<uint64_t, size_t>& counts) {
    counts.clear();
    for (const EntityPtr& e : tracked) {
        if (!e || !e->IsInMap()) {
            continue;
        }
        const Vector3D pos = e->GetPosition();
        MapGrid* grid = map.GetGrid(pos.GridX(), pos.GridY(), pos.GridZ());
        size_t n = 0;
        if (grid) {
            const uint64_t id = e->GetId();
            for (const auto& [gid, ge] : grid->GetEntities()) {
                (void)gid;
                if (ge && ge->GetId() == id) {
                    ++n;
                }
            }
        }
        counts[e->GetId()] = n;
    }
}

// 线性同余伪随机数（LCG），用于压力/浸泡测试确定性随机。
inline uint32_t LcgNext(uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return state;
}

}  // namespace test
