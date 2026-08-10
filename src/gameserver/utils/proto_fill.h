#pragma once

#include <cstdint>

#include <Jolt/Jolt.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>

#include "client_3d.pb.h"
#include "client_common.pb.h"
#include "ecs/components/transform_component.h"

class RoleComponent;

namespace proto_fill {

inline void SetVec3(::vec3* out, const JPH::Vec3& v) {
  if (!out) return;
  out->set_x(v.GetX());
  out->set_y(v.GetY());
  out->set_z(v.GetZ());
}

inline void SetQuat(::quat* out, const JPH::Quat& q) {
  if (!out) return;
  out->set_x(q.GetX());
  out->set_y(q.GetY());
  out->set_z(q.GetZ());
  out->set_w(q.GetW());
}

inline void FillEntityBase(::entity* out, const TransformComponent* tfm) {
  if (!out || !tfm) return;
  SetVec3(out->mutable_pos(), tfm->pos_);
  SetQuat(out->mutable_rot(), tfm->rot_);
  SetQuat(out->mutable_move_rot(), tfm->move_rot_);
  SetVec3(out->mutable_velocity(), tfm->velocity_);
}

inline void FillMoveData(::entity_move_data* out, const TransformComponent* tfm) {
  if (!out || !tfm) return;
  SetVec3(out->mutable_pos(), tfm->pos_);
  SetQuat(out->mutable_rot(), tfm->rot_);
  SetQuat(out->mutable_move_rot(), tfm->move_rot_);
  SetVec3(out->mutable_velocity(), tfm->velocity_);
}

inline void FillJumpData(::entity_jump_data* out, const TransformComponent* tfm) {
  if (!out || !tfm) return;
  out->set_jump_id(tfm->jump_id_);
  out->set_op(static_cast<::jump_op>(tfm->jump_op_));
  out->set_type(static_cast<::jump_type>(tfm->jump_type_));
  out->set_air_jump_index(tfm->air_jump_index_);
  SetVec3(out->mutable_pos(), tfm->pos_);
  SetQuat(out->mutable_rot(), tfm->rot_);
  SetVec3(out->mutable_velocity(), tfm->velocity_);
  out->set_client_time(tfm->jump_client_time_);
}

void FillPlayerData(::entity_player_data* out, uint64_t id,
                    const TransformComponent* tfm, const RoleComponent* role);

}  // namespace proto_fill
