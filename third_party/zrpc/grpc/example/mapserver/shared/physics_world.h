#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace mapserver {

struct Vec3f {
  float x = 0;
  float y = 0;
  float z = 0;
};

struct Quatf {
  float x = 0;
  float y = 0;
  float z = 0;
  float w = 1;
};

struct EntityPhysicsState {
  uint64_t id = 0;
  Vec3f position;
  Quatf rotation;
  Vec3f linear_velocity;
  Vec3f angular_velocity;
};

// Shared Jolt physics world used by both mapserver and client for deterministic
// front-backend validation.
class PhysicsWorld {
 public:
  PhysicsWorld();
  ~PhysicsWorld();

  PhysicsWorld(const PhysicsWorld&) = delete;
  PhysicsWorld& operator=(const PhysicsWorld&) = delete;

  bool CreateSphere(uint64_t id, const Vec3f& position, float radius);
  bool ApplyImpulse(uint64_t id, const Vec3f& impulse);
  void Step(float delta_time, uint32_t collision_steps = 1);
  std::vector<EntityPhysicsState> Snapshot() const;

  static bool NearlyEqual(const EntityPhysicsState& a,
                          const EntityPhysicsState& b, float tolerance);
  static std::string Diff(const EntityPhysicsState& a,
                          const EntityPhysicsState& b);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mapserver
