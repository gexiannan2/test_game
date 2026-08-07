#pragma once

#include "handlers/handler_base.h"

// 连接层：握手 / 心跳 / 重连 / 踢人
class ConnectionHandler : public IHandler {
 public:
  void Handle(const ::zrpc::TcpConnectionPtr& conn,
              const EntityPtr& entity,
              uint32_t msg_id,
              const std::shared_ptr<::google::protobuf::Message>& req) override;
};
