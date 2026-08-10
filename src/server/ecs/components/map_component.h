#pragma once

#include <cstdint>

#include "ecs/component_base/component_base.h"

// 实体所在逻辑地图：cfg_id 配置表，ins_id 运行时实例（副本隔离）。
class MapComponent : public IComponent {
 public:
  ComponentType Type() const override { return ComponentType::kMap; }

  uint64_t map_cfg_id_ = 0;
  uint64_t map_ins_id_ = 0;
};

DECLARE_COMPONENT(MapComponent, ComponentType::kMap)
