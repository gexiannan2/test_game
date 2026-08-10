#pragma once

#include <cstdint>
#include <unordered_map>

#include <Jolt/Jolt.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>

#include "common/aoi_def.h"

class JoltServer;
class Entity;

class JoltSystem {
 public:
    void Bind(JoltServer* server, float tick_interval = 1.0f / 30.0f)
    {
        server_        = server;
        tick_interval_ = tick_interval;
    }

    void OnEntityEnterMap(const EntityPtr& entity);
    void OnEntityLeaveMap(const EntityPtr& entity);
    void OnEntityMove(const EntityPtr& entity);
    void SyncAllBodies(float dt);
    void OnEntityTeleport(const EntityPtr& entity);

    bool HasBody(uint64_t entity_id) const { return entries_.count(entity_id) > 0; }
    size_t GetBodyCount() const { return entries_.size(); }

 private:
    JoltServer* server_              = nullptr;
    float       tick_interval_       = 1.0f / 30.0f;

    struct BodyEntry {
        JPH::BodyID body_id;
        float       radius;
        float       half_height;
        std::weak_ptr<Entity> entity_ref;
    };
    std::unordered_map<uint64_t, BodyEntry> entries_;
};
