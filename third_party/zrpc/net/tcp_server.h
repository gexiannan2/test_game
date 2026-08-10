#pragma once

#include "zrpc/base/thread_pool.h"
#include "zrpc/net/acceptor.h"
#include "zrpc/net/callback.h"
#include <mutex>

namespace zrpc {
class EventLoop;

class TcpServer {
 public:
  typedef std::function<void(EventLoop *)> ThreadInitCallback;

  TcpServer(EventLoop *loop, const std::string &ip, int16_t port,
            const std::any &context);

  ~TcpServer();

  std::shared_ptr<TcpConnection> NewConnection(SocketHandle sockfd);

  bool Start();

  void Stop(std::function<void()> cb = std::function<void()>());

  void RemoveConnection(const std::shared_ptr<TcpConnection> &conn);

  void RemoveConnectionInLoop(const std::shared_ptr<TcpConnection> &conn);

  void StopInLoop();


  void SetAcceptCallback(const AcceptCallback &&cb) {
    accept_callback_ = std::move(cb);
  }

  void SetThreadInitCallback(const ThreadInitCallback &&cb) {
    thread_init_callback_ = std::move(cb);
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

  void SetThreadNum(int16_t num_threads);

  EventLoop *GetLoop() const { return loop_; }

  std::shared_ptr<ThreadPool> GetThreadPool() { return thread_pool_; }

  std::any *GetMutableContext() { return &context_; }

  const std::any &GetContext() const { return context_; }

  void SetContext(const std::any &context) { context_ = context; }

 private:
  TcpServer(const TcpServer &);

  void operator=(const TcpServer &);

  EventLoop *loop_;
  struct CallbackState {
    std::mutex mutex;
    TcpServer *owner{nullptr};
  };
  std::shared_ptr<CallbackState> callback_state_;
  std::unique_ptr<Acceptor> acceptor_;
  std::shared_ptr<ThreadPool> thread_pool_;
  AcceptCallback accept_callback_;
  ConnectionCallback connection_callback_;
  MessageCallback message_callback_;
  WriteCompleteCallback write_complete_callback_;
  ThreadInitCallback thread_init_callback_;

  typedef std::unordered_map<SocketHandle, std::shared_ptr<TcpConnection>>
      ConnectionMap;
  ConnectionMap connections_;
  std::function<void()> stop_callback_;
  std::any context_;
  bool started_{false};
};
}  // namespace zrpc
