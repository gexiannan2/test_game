#pragma once

// 服务端路径移动调度；位移写入场景由 MoveComponent 调 WorldSystem::MoveEntity。

#include <memory>
#include <unordered_set>

#include "common/aoi_def.h"

class WorldSystem;
class MapSystem;
class MoveComponent;

class MoveSystem {
 public:
    void Bind(WorldSystem* world, MapSystem* map);

    void SetMoveSpeed(const EntityPtr& entity, float speed);
    bool RequestMoveTo(const EntityPtr& entity, const Vector3D& destination,
                       MoveCompleteCallback on_complete = nullptr);
    bool IsMoving(const EntityPtr& entity) const;
    void CancelMove(const EntityPtr& entity,
                    MoveStopReason reason = MoveStopReason::kStopCommand);

    void Tick(float dt = 0.0f);

 private:
    void OnMoveComplete(const EntityPtr& entity, bool success,
                        MoveStopReason reason);
    void TickEntityMove(const EntityPtr& entity, float dt);

    WorldSystem* world_ = nullptr;
    MapSystem* map_ = nullptr;
    std::unordered_set<uint64_t> active_movers_;
};
