#pragma once

#include "handlers/handler_base.h"

// 账号登录：首次建实体，重登复用缓存并顶号
class AccountHandler : public IHandler {
 public:
  void Handle(const ::zrpc::TcpConnectionPtr& conn,
              const EntityPtr& entity,
              uint32_t msg_id,
              const std::shared_ptr<::google::protobuf::Message>& req) override;
};
