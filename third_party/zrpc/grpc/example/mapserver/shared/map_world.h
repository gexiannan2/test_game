#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "shared/physics_world.h"

namespace mapserver {

// Authoritative map + physics state shared by server and client.
class MapWorld {
 public:
  MapWorld();

  bool CreateEntity(uint64_t id, const Vec3f& position, float radius);
  bool ApplyImpulse(uint64_t id, const Vec3f& impulse);
  void Step(float delta_time, uint32_t collision_steps = 1);

  uint64_t tick() const { return tick_; }
  std::vector<EntityPhysicsState> Snapshot() const;

  bool ValidateAgainst(const std::vector<EntityPhysicsState>& client_states,
                       float tolerance, std::string* diff) const;

 private:
  PhysicsWorld physics_;
  uint64_t tick_ = 0;
};

}  // namespace mapserver
