#include "shared/map_world.h"

#include <sstream>
#include <unordered_map>

namespace mapserver {

MapWorld::MapWorld() = default;

bool MapWorld::CreateEntity(uint64_t id, const Vec3f& position, float radius) {
  return physics_.CreateSphere(id, position, radius);
}

bool MapWorld::ApplyImpulse(uint64_t id, const Vec3f& impulse) {
  return physics_.ApplyImpulse(id, impulse);
}

void MapWorld::Step(float delta_time, uint32_t collision_steps) {
  physics_.Step(delta_time, collision_steps);
  ++tick_;
}

std::vector<EntityPhysicsState> MapWorld::Snapshot() const {
  return physics_.Snapshot();
}

bool MapWorld::ValidateAgainst(
    const std::vector<EntityPhysicsState>& client_states, float tolerance,
    std::string* diff) const {
  const auto server_states = Snapshot();
  std::unordered_map<uint64_t, EntityPhysicsState> server_map;
  for (const auto& state : server_states) {
    server_map[state.id] = state;
  }

  std::ostringstream oss;
  bool match = true;
  for (const auto& client : client_states) {
    auto it = server_map.find(client.id);
    if (it == server_map.end()) {
      match = false;
      oss << "missing server entity id=" << client.id << "; ";
      continue;
    }
    if (!PhysicsWorld::NearlyEqual(client, it->second, tolerance)) {
      match = false;
      oss << PhysicsWorld::Diff(client, it->second) << "; ";
    }
  }

  for (const auto& server : server_states) {
    bool found = false;
    for (const auto& client : client_states) {
      if (client.id == server.id) {
        found = true;
        break;
      }
    }
    if (!found) {
      match = false;
      oss << "missing client entity id=" << server.id << "; ";
    }
  }

  if (diff != nullptr) {
    *diff = oss.str();
  }
  return match;
}

}  // namespace mapserver
