#pragma once
#include <memory>
#include <string>
#include <functional>

#include "zrpc/net/socket.h"

namespace zrpc {
class TcpConnection;
class Buffer;
//class Message;

typedef std::function<void()> TimerCallback;
typedef std::function<void(const std::shared_ptr<TcpConnection> &)> ConnectionCallback;
typedef std::function<void(const std::shared_ptr<TcpConnection> &)> DisConnectionCallback;
typedef std::function<void()> ConnectionErrorCallback;
typedef std::function<void(SocketHandle)> AcceptCallback;
typedef std::function<void(const std::shared_ptr<TcpConnection> &)> CloseCallback;
typedef std::function<void(const std::shared_ptr<TcpConnection> &)> WriteCompleteCallback;
typedef std::function<void(const std::shared_ptr<TcpConnection> &, size_t)>
    HighWaterMarkCallback;
typedef std::function<void(const std::shared_ptr<TcpConnection> &, Buffer *)> MessageCallback;
//typedef std::function<void(const std::shared_ptr<TcpConnection> &, const Message& )>
//    Message1Callback;
}  // namespace zrpc
