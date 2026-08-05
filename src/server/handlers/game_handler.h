#pragma once

#include "handlers/handler_base.h"

// 进游戏：出生点、EnterMap、推自身 appear
class GameHandler : public IHandler {
 public:
  void Handle(const ::zrpc::TcpConnectionPtr& conn,
              const EntityPtr& entity,
              uint32_t msg_id,
              const std::shared_ptr<::google::protobuf::Message>& req) override;
};
