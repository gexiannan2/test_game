#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Quat.h>

#include "ecs/component_base/component_base.h"

// 3D 变换：pos_ 为脚底世界坐标，供 AOI/物理/移动系统读取。
// scale_ 为占地大小（城池等大物体用），collision_ 为碰撞半径（绕障寻路用）。
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
};

DECLARE_COMPONENT(TransformComponent, ComponentType::kTransform)
