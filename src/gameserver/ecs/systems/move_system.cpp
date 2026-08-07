// 路径移动实现（摇杆移动走 handler → MoveEntity，不经本类）。

#include "ecs/systems/move_system.h"

#include <vector>

#include "ecs/components/move_component.h"
#include "ecs/components/transform_component.h"
#include "ecs/systems/world_system.h"
#include "ecs/systems/map_system.h"

void MoveSystem::Bind(WorldSystem* world, MapSystem* map) {
    world_ = world;
    map_ = map;
}

void MoveSystem::SetMoveSpeed(const EntityPtr& entity, float speed) {
    MoveComponent* move = entity ? entity->GetComponent<MoveComponent>() : nullptr;
    if (move) {
        move->SetSpeed(speed);
    }
}

bool MoveSystem::RequestMoveTo(const EntityPtr& entity,
                                const Vector3D& destination,
                                MoveCompleteCallback on_complete) {
    if (!entity || !entity->IsInMap() || !map_ || !map_->IsInMap(destination)) {
        return false;
    }
    MoveComponent* move = entity->GetComponent<MoveComponent>();
    if (!move) {
        return false;
    }

    EntityPtr keep = entity;
    const bool ok = move->MoveTo(
        destination, [this, keep, cb = std::move(on_complete)](
                         bool success, MoveStopReason reason) {
            OnMoveComplete(keep, success, reason);
            if (cb) {
                cb(keep, success, reason);
            }
        });
    if (ok) {
        active_movers_.insert(entity->GetId());
    }
    return ok;
}

bool MoveSystem::IsMoving(const EntityPtr& entity) const {
    const MoveComponent* move =
        entity ? entity->GetComponent<MoveComponent>() : nullptr;
    return move && move->IsMoving();
}

void MoveSystem::CancelMove(const EntityPtr& entity, MoveStopReason reason) {
    if (!entity) {
        return;
    }
    MoveComponent* move = entity->GetComponent<MoveComponent>();
    if (move && move->IsMoving()) {
        entity->SetPropertyDirty(EntityPropertyType::kStopMove);
        move->OnStopMove(false, reason);
    }
    active_movers_.erase(entity->GetId());
}

void MoveSystem::Tick(float dt) {
    std::vector<uint64_t> ids(active_movers_.begin(), active_movers_.end());
    for (uint64_t id : ids) {
        EntityPtr entity = world_->FindEntity(id);
        if (!entity) {
            active_movers_.erase(id);
            continue;
        }
        if (MoveComponent* move = entity->GetComponent<MoveComponent>()) {
            if (move->IsMoving()) {
                TickEntityMove(entity, dt);
            } else {
                active_movers_.erase(id);
            }
        }
    }
}

void MoveSystem::TickEntityMove(const EntityPtr& entity, float dt) {
    if (!entity || !world_) {
        return;
    }
    if (MoveComponent* move = entity->GetComponent<MoveComponent>()) {
        move->TickFrame(world_, entity.get(), dt > 0.f ? dt : (1.f / 60.f));
    }
}

void MoveSystem::OnMoveComplete(const EntityPtr& entity, bool /*success*/,
                                MoveStopReason /*reason*/) {
    if (entity) {
        active_movers_.erase(entity->GetId());
    }
}


