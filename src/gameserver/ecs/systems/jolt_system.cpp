#include "ecs/systems/jolt_system.h"

#include <Jolt/Physics/Body/BodyID.h>

#include "ecs/entity/entity.h"
#include "jolt_server.h"
#include "zrpc/base/logger.h"

void JoltSystem::OnEntityEnterMap(const EntityPtr& entity)
{
    if (!server_ || !server_->IsMapLoaded()) return;
    if (!entity) return;

    uint64_t eid = entity->GetId();
    if (entries_.count(eid)) return;

    entity->InitCapsuleParams();

    float half_h = entity->GetCapsuleHalfHeight();
    float radius = entity->GetRadius();
    JPH::Vec3 center = entity->GetBodyCenter();

    auto* shape = new JPH::CapsuleShape(half_h, radius);

    JPH::BodyID body_id = server_->CreateKinematicBody(shape, center, entity->GetDirection());
    if (!body_id.IsInvalid())
    {
        entries_[eid] = {body_id, radius, half_h, std::weak_ptr<Entity>(entity)};
        LOG_INFO << "[JOLT] capsule created  " << entity->LogTag()
                 << " body_id=" << body_id.GetIndexAndSequenceNumber()
                 << " radius=" << radius << " height=" << entity->GetHeight()
                 << " center=(" << center.GetX() << "," << center.GetY()
                 << "," << center.GetZ() << ")";
    }
    else
    {
        LOG_WARN << "[JOLT] capsule creation FAILED  " << entity->LogTag();
    }
}

void JoltSystem::OnEntityLeaveMap(const EntityPtr& entity)
{
    if (!server_ || !entity) return;

    uint64_t eid = entity->GetId();
    auto it = entries_.find(eid);
    if (it == entries_.end()) return;

    server_->RemoveBody(it->second.body_id);
    entries_.erase(it);
}

void JoltSystem::OnEntityMove(const EntityPtr& entity)
{
    (void)entity;
}

void JoltSystem::SyncAllBodies(float dt)
{
    if (!server_) return;
    for (auto& [eid, entry] : entries_)
    {
        auto entity = entry.entity_ref.lock();
        if (!entity) continue;

        JPH::Vec3 target_center = entity->GetBodyCenter();
        server_->MoveKinematicBody(entry.body_id, target_center, entity->GetDirection(), dt);
    }
}

void JoltSystem::OnEntityTeleport(const EntityPtr& entity)
{
    if (!server_ || !entity) return;

    uint64_t eid = entity->GetId();
    auto it = entries_.find(eid);
    if (it == entries_.end()) return;

    JPH::Vec3 center = entity->GetBodyCenter();
    server_->SetBodyPosition(it->second.body_id, center);
}
