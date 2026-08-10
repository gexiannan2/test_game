#pragma once

#include "handlers/handler_base.h"

class MapConfig;
class ConnectionComponent;

// 进游戏：出生点、EnterMap、推自身 appear
class GameHandler : public IHandler {
 public:
  void Handle(const ::zrpc::TcpConnectionPtr& conn,
              const EntityPtr& entity,
              uint32_t msg_id,
              const std::shared_ptr<::google::protobuf::Message>& req) override;

 private:
  // 非首次进图（断线重连后重进）：内存实体仍在，用当前位置直接进图。
  void HandleReEnter(const ::zrpc::TcpConnectionPtr& conn,
                     const EntityPtr& entity,
                     ConnectionComponent* conn_comp);

  // 首次进图：异步加载 DB 存档后再进图。
  void HandleFirstEnter(const ::zrpc::TcpConnectionPtr& conn,
                        const EntityPtr& entity,
                        ConnectionComponent* conn_comp,
                        uint64_t role_id,
                        const MapConfig* default_map);
};
