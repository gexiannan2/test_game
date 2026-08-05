#pragma once

#include "common/vector3d.h"
#include "jolt_server.h"

// 地图 AABB 钳制：XZ/Y 均向内收 margin，禁止穿出地板/天花板与水平边界。
// game_server（WorldSystem::SetBoundsClamp）与 move_handler 共用，避免双份逻辑漂移。
namespace server {

inline constexpr float kDefaultBoundsMargin = 1.0f;

inline Vector3D ClampToMapBounds(const MapBounds& b, const Vector3D& pos,
                                 float margin = kDefaultBoundsMargin) {
  Vector3D c = pos;
  if (c.GetX() < b.min.GetX() + margin) {
    c.SetX(b.min.GetX() + margin);
  } else if (c.GetX() > b.max.GetX() - margin) {
    c.SetX(b.max.GetX() - margin);
  }
  if (c.GetZ() < b.min.GetZ() + margin) {
    c.SetZ(b.min.GetZ() + margin);
  } else if (c.GetZ() > b.max.GetZ() - margin) {
    c.SetZ(b.max.GetZ() - margin);
  }
  if (c.GetY() < b.min.GetY() + margin) {
    c.SetY(b.min.GetY() + margin);
  } else if (c.GetY() > b.max.GetY() - margin) {
    c.SetY(b.max.GetY() - margin);
  }
  return c;
}

}  // namespace server
