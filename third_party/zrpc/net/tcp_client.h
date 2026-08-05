#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <functional>
#include <any>
#include <mutex>

#include "zrpc/net/callback.h"
#include "zrpc/net/connector.h"
#include "zrpc/net/event_loop.h"

namespace zrpc {
class TcpClient {
 public:
  TcpClient(EventLoop *loop, const std::string &ip, int16_t port,
            const std::any &context);

  ~TcpClient();

  void Connect(bool state = false);
  void DisConnect();
  void Stop();

  bool GetRetry() { return retry_.load(std::memory_order_acquire); }
  void EnableRetry();
  void CloseRetry();

  void SetConnectionErrorCallBack(const ConnectionErrorCallback &&cb) {
    connection_error_callBack_ = std::move(cb);
  }

  void SetConnectionCallback(const ConnectionCallback &&cb) {
    connection_callback_ = std::move(cb);
  }

  void SetMessageCallback(const MessageCallback &&cb) {
    message_callback_ = std::move(cb);
  }

  void SetWriteCompleteCallback(const WriteCompleteCallback &&cb) {
    write_complete_callback_ = std::move(cb);
  }

  EventLoop *GetLoop() { return loop_; }
  std::any *GetContext() { return &context_; }
  const std::any &GetContext() const { return context_; }
  void SetContext(const std::any &context) { context_ = context; }

  std::string GetIp() { return ip_; }
  int16_t GetPort() { return port_; }
  std::shared_ptr<TcpConnection> GetConnection();
  void HandlePeerClose(const std::shared_ptr<TcpConnection>& conn);

 private:
  TcpClient(const TcpClient &);

  void operator=(const TcpClient &);

  std::shared_ptr<TcpConnection> NewConnection(SocketHandle sockfd);
  void RemoveConnection(const std::shared_ptr<TcpConnection> &conn);

  std::shared_ptr<Connector> connector_;
  EventLoop *loop_;

  struct CallbackState {
    // 仅保护跨线程析构与 Connector 回调之间的 owner 生命周期，不代表 TcpClient 全体 API 线程安全。
    std::mutex mutex;
    TcpClient *owner{nullptr};
  };
  std::shared_ptr<CallbackState> callback_state_;

  std::mutex mutex_;
  ConnectionErrorCallback connection_error_callBack_;
  ConnectionCallback connection_callback_;
  MessageCallback message_callback_;
  WriteCompleteCallback write_complete_callback_;

  std::shared_ptr<TcpConnection> connection_;
  std::any context_;
  std::string ip_;
  int16_t port_;
  std::atomic<bool> retry_;
  std::atomic<bool> connecting_;
};
}  // namespace zrpc
