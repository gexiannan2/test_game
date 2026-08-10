#pragma once

#include <cstdint>
#include <string>

#include "ecs/component_base/component_base.h"

// 平台账号绑定：登录成功后写入，与 ConnectionComponent 协同存在。
class AccountComponent : public IComponent {
 public:
  ComponentType Type() const override { return ComponentType::kAccount; }

  std::string uid_;
  std::string token_;
  uint32_t channel_id_ = 0;
  uint64_t session_id_ = 0;
};

DECLARE_COMPONENT(AccountComponent, ComponentType::kAccount)
