#include "shared/physics_world.h"

#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <cmath>
#include <sstream>
#include <thread>

JPH_SUPPRESS_WARNINGS

namespace mapserver {
namespace {

using namespace JPH;
using namespace JPH::literals;

namespace Layers {
static constexpr ObjectLayer kNonMoving = 0;
static constexpr ObjectLayer kMoving = 1;
static constexpr ObjectLayer kNumLayers = 2;
}  // namespace Layers

class ObjectLayerPairFilterImpl final : public ObjectLayerPairFilter {
 public:
  bool ShouldCollide(ObjectLayer inObject1,
                     ObjectLayer inObject2) const override {
    switch (inObject1) {
      case Layers::kNonMoving:
        return inObject2 == Layers::kMoving;
      case Layers::kMoving:
        return true;
      default:
        return false;
    }
  }
};

namespace BroadPhaseLayers {
static constexpr BroadPhaseLayer kNonMoving(0);
static constexpr BroadPhaseLayer kMoving(1);
static constexpr uint kNumLayers(2);
}  // namespace BroadPhaseLayers

class BPLayerInterfaceImpl final : public BroadPhaseLayerInterface {
 public:
  BPLayerInterfaceImpl() {
    mObjectToBroadPhase[Layers::kNonMoving] = BroadPhaseLayers::kNonMoving;
    mObjectToBroadPhase[Layers::kMoving] = BroadPhaseLayers::kMoving;
  }

  uint GetNumBroadPhaseLayers() const override {
    return BroadPhaseLayers::kNumLayers;
  }

  BroadPhaseLayer GetBroadPhaseLayer(ObjectLayer inLayer) const override {
    return mObjectToBroadPhase[inLayer];
  }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
  const char* GetBroadPhaseLayerName(BroadPhaseLayer inLayer) const override {
    switch ((BroadPhaseLayer::Type)inLayer) {
      case (BroadPhaseLayer::Type)BroadPhaseLayers::kNonMoving:
        return "NON_MOVING";
      case (BroadPhaseLayer::Type)BroadPhaseLayers::kMoving:
        return "MOVING";
      default:
        return "INVALID";
    }
  }
#endif

 private:
  BroadPhaseLayer mObjectToBroadPhase[Layers::kNumLayers];
};

class ObjectVsBroadPhaseLayerFilterImpl final
    : public ObjectVsBroadPhaseLayerFilter {
 public:
  bool ShouldCollide(ObjectLayer inLayer1,
                     BroadPhaseLayer inLayer2) const override {
    switch (inLayer1) {
      case Layers::kNonMoving:
        return inLayer2 == BroadPhaseLayers::kMoving;
      case Layers::kMoving:
        return true;
      default:
        return false;
    }
  }
};

Vec3 ToJolt(const Vec3f& v) { return Vec3(v.x, v.y, v.z); }

Vec3f FromJolt(const Vec3& v) { return Vec3f{v.GetX(), v.GetY(), v.GetZ()}; }

Quatf FromJolt(const Quat& q) {
  return Quatf{q.GetX(), q.GetY(), q.GetZ(), q.GetW()};
}

}  // namespace

struct PhysicsWorld::Impl {
  bool jolt_ready = false;
  std::unique_ptr<TempAllocatorImpl> temp_allocator;
  std::unique_ptr<JobSystemThreadPool> job_system;
  std::unique_ptr<PhysicsSystem> physics_system;
  BPLayerInterfaceImpl broad_phase_layer_interface;
  ObjectVsBroadPhaseLayerFilterImpl object_vs_broadphase_layer_filter;
  ObjectLayerPairFilterImpl object_vs_object_layer_filter;
  std::unordered_map<uint64_t, BodyID> bodies;
};

PhysicsWorld::PhysicsWorld() : impl_(new Impl) {
  RegisterDefaultAllocator();
  Factory::sInstance = new Factory();
  RegisterTypes();

  impl_->temp_allocator.reset(new TempAllocatorImpl(10 * 1024 * 1024));
  const uint threads =
      std::max(1u, std::thread::hardware_concurrency() > 0
                       ? std::thread::hardware_concurrency() - 1
                       : 1u);
  impl_->job_system.reset(
      new JobSystemThreadPool(cMaxPhysicsJobs, cMaxPhysicsBarriers, threads));

  impl_->physics_system.reset(new PhysicsSystem);
  impl_->physics_system->Init(
      4096, 0, 4096, 4096, impl_->broad_phase_layer_interface,
      impl_->object_vs_broadphase_layer_filter,
      impl_->object_vs_object_layer_filter);

  BodyInterface& body_interface = impl_->physics_system->GetBodyInterface();
  BodyCreationSettings floor_settings(
      new BoxShape(Vec3(100.0f, 1.0f, 100.0f)), RVec3(0.0_r, -1.0_r, 0.0_r),
      Quat::sIdentity(), EMotionType::Static, Layers::kNonMoving);
  Body* floor = body_interface.CreateBody(floor_settings);
  body_interface.AddBody(floor->GetID(), EActivation::DontActivate);
  impl_->jolt_ready = true;
}

PhysicsWorld::~PhysicsWorld() {
  if (!impl_->jolt_ready) {
    return;
  }

  BodyInterface& body_interface = impl_->physics_system->GetBodyInterface();
  for (const auto& item : impl_->bodies) {
    body_interface.RemoveBody(item.second);
    body_interface.DestroyBody(item.second);
  }
  impl_->bodies.clear();

  UnregisterTypes();
  delete Factory::sInstance;
  Factory::sInstance = nullptr;
}

bool PhysicsWorld::CreateSphere(uint64_t id, const Vec3f& position,
                                float radius) {
  if (!impl_->jolt_ready || impl_->bodies.count(id) > 0 || radius <= 0.0f) {
    return false;
  }

  BodyInterface& body_interface = impl_->physics_system->GetBodyInterface();
  BodyCreationSettings settings(
      new SphereShape(radius), RVec3(position.x, position.y, position.z),
      Quat::sIdentity(), EMotionType::Dynamic, Layers::kMoving);
  settings.mUserData = id;
  BodyID body_id = body_interface.CreateAndAddBody(settings, EActivation::Activate);
  if (body_id.IsInvalid()) {
    return false;
  }
  impl_->bodies[id] = body_id;
  return true;
}

bool PhysicsWorld::ApplyImpulse(uint64_t id, const Vec3f& impulse) {
  auto it = impl_->bodies.find(id);
  if (it == impl_->bodies.end()) {
    return false;
  }
  BodyInterface& body_interface = impl_->physics_system->GetBodyInterface();
  body_interface.ActivateBody(it->second);
  body_interface.AddImpulse(it->second, ToJolt(impulse));
  return true;
}

void PhysicsWorld::Step(float delta_time, uint32_t collision_steps) {
  if (!impl_->jolt_ready || delta_time <= 0.0f) {
    return;
  }
  const int steps = static_cast<int>(std::max<uint32_t>(1, collision_steps));
  impl_->physics_system->Update(delta_time, steps, impl_->temp_allocator.get(),
                                impl_->job_system.get());
}

std::vector<EntityPhysicsState> PhysicsWorld::Snapshot() const {
  std::vector<EntityPhysicsState> states;
  states.reserve(impl_->bodies.size());
  BodyInterface& body_interface =
      const_cast<PhysicsSystem*>(impl_->physics_system.get())
          ->GetBodyInterface();

  for (const auto& item : impl_->bodies) {
    const BodyLockRead lock(impl_->physics_system->GetBodyLockInterface(),
                            item.second);
    if (!lock.Succeeded()) {
      continue;
    }
    const Body& body = lock.GetBody();
    EntityPhysicsState state;
    state.id = item.first;
    state.position = FromJolt(body.GetPosition());
    state.rotation = FromJolt(body.GetRotation());
    state.linear_velocity = FromJolt(body.GetLinearVelocity());
    state.angular_velocity = FromJolt(body.GetAngularVelocity());
    states.push_back(state);
  }
  return states;
}

bool PhysicsWorld::NearlyEqual(const EntityPhysicsState& a,
                               const EntityPhysicsState& b, float tolerance) {
  auto close = [tolerance](float x, float y) {
    return std::fabs(x - y) <= tolerance;
  };
  return close(a.position.x, b.position.x) && close(a.position.y, b.position.y) &&
         close(a.position.z, b.position.z) &&
         close(a.linear_velocity.x, b.linear_velocity.x) &&
         close(a.linear_velocity.y, b.linear_velocity.y) &&
         close(a.linear_velocity.z, b.linear_velocity.z);
}

std::string PhysicsWorld::Diff(const EntityPhysicsState& a,
                               const EntityPhysicsState& b) {
  std::ostringstream oss;
  oss << "id=" << a.id << " pos=(" << a.position.x << "," << a.position.y << ","
      << a.position.z << ") vs (" << b.position.x << "," << b.position.y << ","
      << b.position.z << ") vel=(" << a.linear_velocity.x << ","
      << a.linear_velocity.y << "," << a.linear_velocity.z << ") vs ("
      << b.linear_velocity.x << "," << b.linear_velocity.y << ","
      << b.linear_velocity.z << ")";
  return oss.str();
}

}  // namespace mapserver
