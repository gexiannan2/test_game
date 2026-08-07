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
  // account_info 表的数字主键（首次登录时由 mongo 层生成并回写）。
  // PostOne 落地 players 文档时作为顶层 account_id 字段写入，
  // 用于"一个账号多个角色"场景下从 account_id 反查所有 players。
  // 0 = 尚未从 mongo 拿到（首次登录或重连未完成异步回调）。
  int64_t account_id_ = 0;
};

DECLARE_COMPONENT(AccountComponent, ComponentType::kAccount)
