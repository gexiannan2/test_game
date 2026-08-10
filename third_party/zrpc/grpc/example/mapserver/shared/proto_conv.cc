#include "shared/proto_conv.h"

namespace mapserver {

Vec3f FromProto(const ::mapserver::Vec3& v) {
  return Vec3f{v.x(), v.y(), v.z()};
}

Quatf FromProto(const ::mapserver::Quat& q) {
  return Quatf{q.x(), q.y(), q.z(), q.w()};
}

::mapserver::Vec3 ToProto(const Vec3f& v) {
  ::mapserver::Vec3 out;
  out.set_x(v.x);
  out.set_y(v.y);
  out.set_z(v.z);
  return out;
}

::mapserver::Quat ToProto(const Quatf& q) {
  ::mapserver::Quat out;
  out.set_x(q.x);
  out.set_y(q.y);
  out.set_z(q.z);
  out.set_w(q.w);
  return out;
}

void FillSnapshot(uint64_t tick, const std::vector<EntityPhysicsState>& states,
                  ::mapserver::WorldSnapshot* snapshot) {
  snapshot->set_tick(tick);
  snapshot->clear_entities();
  for (const auto& state : states) {
    auto* entity = snapshot->add_entities();
    entity->set_id(state.id);
    *entity->mutable_position() = ToProto(state.position);
    *entity->mutable_rotation() = ToProto(state.rotation);
    *entity->mutable_linear_velocity() = ToProto(state.linear_velocity);
    *entity->mutable_angular_velocity() = ToProto(state.angular_velocity);
  }
}

std::vector<EntityPhysicsState> FromSnapshot(
    const ::mapserver::WorldSnapshot& snapshot) {
  std::vector<EntityPhysicsState> states;
  states.reserve(snapshot.entities_size());
  for (const auto& entity : snapshot.entities()) {
    EntityPhysicsState state;
    state.id = entity.id();
    state.position = FromProto(entity.position());
    state.rotation = FromProto(entity.rotation());
    state.linear_velocity = FromProto(entity.linear_velocity());
    state.angular_velocity = FromProto(entity.angular_velocity());
    states.push_back(state);
  }
  return states;
}

}  // namespace mapserver
