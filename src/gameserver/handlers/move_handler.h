#pragma once

#include "handlers/handler_base.h"

// 3D 移动：先 MoveEntity 再 OnMove（顺序不能反）
class MoveHandler : public IHandler {
 public:
  void Handle(const ::zrpc::TcpConnectionPtr& conn,
              const EntityPtr& entity,
              uint32_t msg_id,
              const std::shared_ptr<::google::protobuf::Message>& req) override;
};
