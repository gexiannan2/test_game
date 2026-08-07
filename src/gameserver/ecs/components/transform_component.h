#pragma once

#include <cstdint>

#include <Jolt/Jolt.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Quat.h>

#include "ecs/component_base/component_base.h"

// 3D 变换：pos_ 为脚底世界坐标，供 AOI/物理/移动系统读取。
// scale_ 为占地大小（城池等大物体用），collision_ 为碰撞半径（绕障寻路用）。
// jump_* 为跳跃状态，由 jump_handler 写入，SerializeDirty(kJump) 读取广播。
class TransformComponent : public IComponent {
 public:
  ComponentType Type() const override { return ComponentType::kTransform; }

  JPH::Vec3 pos_       = JPH::Vec3::sZero();
  JPH::Quat rot_       = JPH::Quat::sIdentity();
  JPH::Quat move_rot_  = JPH::Quat::sIdentity();
  JPH::Vec3 velocity_  = JPH::Vec3::sZero();
  float height_        = 1.5f;
  float radius_        = 0.3f;
  int32_t scale_       = 0;
  int32_t collision_   = 0;

  // 跳跃/空中状态（jump_handler 写入，SerializeDirty kJump 读取）
  uint32_t jump_id_          = 0;   // 空中会话 ID
  int32_t  jump_op_          = 0;   // jump_op: START/STEER/LAND
  int32_t  jump_type_        = 0;   // jump_type: NORMAL/DOUBLE/FALL/SLIDE/DRAGON
  uint32_t air_jump_index_   = 0;   // 已用空中起跳次数（含首段）
  bool     airborne_         = false; // 是否处于空中会话（跳/落/滑）
  uint64_t jump_client_time_ = 0;

  void ResetAirState() {
    jump_id_ = 0;
    jump_op_ = 0;
    jump_type_ = 0;
    air_jump_index_ = 0;
    airborne_ = false;
    jump_client_time_ = 0;
  }
};

DECLARE_COMPONENT(TransformComponent, ComponentType::kTransform)
