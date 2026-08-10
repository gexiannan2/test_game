#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "ecs/entity/entity.h"
#include "protocol/pack_codec.h"

namespace zrpc {
class TcpConnection;
using TcpConnectionPtr = std::shared_ptr<TcpConnection>;
}  // namespace zrpc

namespace google {
namespace protobuf {
class Message;
}  // namespace protobuf
}  // namespace google

class GameServer;

// 协议 Handler 基类：框架层已完成反序列化，Handler 收到已解析的 protobuf 对象。
// req 为 nullptr 表示该消息无请求体（如心跳/进游戏请求）。
class IHandler {
 public:
  virtual ~IHandler() = default;

  void SetServer(GameServer* server) { server_ = server; }

  virtual void Handle(const ::zrpc::TcpConnectionPtr& conn,
                      const EntityPtr& entity,
                      uint32_t msg_id,
                      const std::shared_ptr<::google::protobuf::Message>& req) = 0;

 protected:
  GameServer* server_ = nullptr;
};

using HandlerFactory = std::function<std::unique_ptr<IHandler>()>;
