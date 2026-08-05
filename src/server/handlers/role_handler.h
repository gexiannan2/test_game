#pragma once

#include "handlers/handler_base.h"

// 角色：列表 / 创建 / 删除 / 登录（含 session 恢复）
class RoleHandler : public IHandler {
 public:
  void Handle(const ::zrpc::TcpConnectionPtr& conn,
              const EntityPtr& entity,
              uint32_t msg_id,
              const std::shared_ptr<::google::protobuf::Message>& req) override;
};
