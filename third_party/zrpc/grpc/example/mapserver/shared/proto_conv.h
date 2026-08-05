#pragma once

#include <vector>

#include "mapserver.pb.h"
#include "shared/physics_world.h"

namespace mapserver {

Vec3f FromProto(const ::mapserver::Vec3& v);
Quatf FromProto(const ::mapserver::Quat& q);
::mapserver::Vec3 ToProto(const Vec3f& v);
::mapserver::Quat ToProto(const Quatf& q);

void FillSnapshot(uint64_t tick, const std::vector<EntityPhysicsState>& states,
                  ::mapserver::WorldSnapshot* snapshot);
std::vector<EntityPhysicsState> FromSnapshot(
    const ::mapserver::WorldSnapshot& snapshot);

}  // namespace mapserver
