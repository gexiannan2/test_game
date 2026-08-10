#pragma once

#include <atomic>
#include <assert.h>
#include <string>
#include <memory>
#include <vector>
#include <functional>

#include "zrpc/base/logger.h"
#include "zrpc/net/channel.h"
#include "zrpc/net/event_loop.h"
#include "zrpc/net/socket.h"

namespace zrpc {
class Connector : public std::enable_shared_from_this<Connector> {
 public:
  typedef std::function<void(SocketHandle)> NewConnectionCallback;
  typedef std::function<void()> ErrorConnectionCallback;

  Connector(EventLoop *loop, const char *ip, int16_t port, bool retry);

  ~Connector();

  void SetNewConnectionCallback(const NewConnectionCallback &&cb) {
    new_connection_callback_ = std::move(cb);
  }

  void SetConnectionErrorCallBack(const ErrorConnectionCallback &&cb) {
    error_connection_callback_ = std::move(cb);
  }

  void Start(bool state);

  void Restart();

  void Stop();

  void EnableRetry() { retry_ = true; }

  void CloseRetry() { retry_ = false; }

 private:
  Connector(const Connector &);

  void operator=(const Connector &);

  void StartInLoop(bool s);

  void StopInLoop();

  void Connecting(bool state);

  void Connecting(bool state, SocketHandle sockfd);

  void ResetChannel();

  void Retry(SocketHandle sockfd);

  void HandleWrite();

  void HandleError();

  SocketHandle RemoveAndResetChannel();

#ifdef _WIN32
  void PollConnect();
  void FinishConnect(SocketHandle sockfd);
#endif

  enum States { kDisconnected, kConnecting, kConnected };

  void SetState(States state) { state_ = state; }

  static const int kMaxRetryDelayMs = 30 * 1000;
  static const int kInitRetryDelayMs = 1000;
  static const int kHeart = 5;

  EventLoop *loop_;
  std::string ip_;
  int16_t port_;
  std::atomic<bool> connect_;
  std::atomic<bool> retry_;
  int32_t retry_delay_ms_;
  States state_;
  std::unique_ptr<Channel> channel_;
#ifdef _WIN32
  SocketHandle connect_sockfd_{kInvalidSocket};
#endif

  ErrorConnectionCallback error_connection_callback_;
  NewConnectionCallback new_connection_callback_;
};

}  // namespace zrpc
