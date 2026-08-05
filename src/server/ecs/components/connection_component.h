#pragma once

#include <cstdint>
#include <memory>

#include "ecs/component_base/component_base.h"

namespace zrpc {
class TcpConnection;
using TcpConnectionPtr = std::shared_ptr<TcpConnection>;
}  // namespace zrpc

// TCP 会话绑定：发包、心跳、断线清理均通过 conn_。
class ConnectionComponent : public IComponent {
 public:
  ComponentType Type() const override { return ComponentType::kConnection; }

  ::zrpc::TcpConnectionPtr conn_;
  uint8_t send_seq_ = 0;
  uint64_t last_heartbeat_sec_ = 0;
};

DECLARE_COMPONENT(ConnectionComponent, ComponentType::kConnection)
