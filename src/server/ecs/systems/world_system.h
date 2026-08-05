#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <Jolt/Jolt.h>
#include <Jolt/Math/Vec3.h>

#include "common/aoi_def.h"
#include "ecs/systems/map_grid.h"
#include "ecs/systems/map_system.h"
#include "ecs/systems/aoi_system.h"
#include "ecs/systems/move_system.h"
#include "ecs/systems/jolt_system.h"

class JoltServer;

// 场景总线：Map + AOI + Move，对外唯一进/离/移动入口。
class WorldSystem {
 public:
    static std::shared_ptr<WorldSystem> Create(SceneRegionType system_type);

    void Init();
    void Tick(float dt = 1.0f / 30.0f);

    void SetJoltServer(JoltServer* server) { jolt_server_ = server; }

    void SetEntityFactory(EntityFactory factory);

    EntityPtr Spawn(EntityType type, const EntitySpawn& spawn);
    EntityPtr SpawnOnMap(EntityType type, const EntitySpawn& spawn);

    void EnterMap(const EntityPtr& entity);
    void LeaveMap(const EntityPtr& entity);
    void MoveEntity(const EntityPtr& entity, const Vector3D& new_pos);
    void UpdateEntity(const EntityPtr& entity);

    std::vector<uint64_t> GetVisibleEntities(uint64_t watcher_id) const;
    bool IsWatcher(uint64_t entity_id) const;

    uint64_t AllocateEntityId() { return next_entity_id_++; }

    void RegisterEntity(const EntityPtr& entity);
    void UnregisterEntity(uint64_t entity_id);
    EntityPtr FindEntity(uint64_t entity_id) const;
    size_t GetEntityCount() const;

    MapSystem& Map() { return *map_; }
    AoiSystem& Aoi() { return aoi_; }
    MoveSystem& Move() { return full_move_system_; }
    JoltSystem& Jolt() { return jolt_system_; }

    // 设置地图边界钳制回调（Jolt OBJ AABB），MoveEntity 每帧调用
    using BoundsClampFn = std::function<Vector3D(const Vector3D&)>;
    void SetBoundsClamp(BoundsClampFn fn) { bounds_clamp_ = std::move(fn); }

 private:
    WorldSystem(SceneRegionType system_type);

    bool initialized_ = false;
    EntityFactory factory_;
    uint64_t next_entity_id_ = 1;

    std::unique_ptr<MapSystem> map_;
    AoiSystem aoi_;
    MoveSystem full_move_system_;
    JoltSystem jolt_system_;
    JoltServer* jolt_server_ = nullptr;  // 非拥有，Tick 中调 Update

    BoundsClampFn bounds_clamp_;

    mutable std::mutex entity_mutex_;
    std::unordered_map<uint64_t, std::weak_ptr<Entity>> entity_index_;
};
