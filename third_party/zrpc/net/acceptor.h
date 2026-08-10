#pragma once

#include <assert.h>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "zrpc/net/channel.h"
#include "zrpc/net/event_loop.h"
#include "zrpc/net/socket.h"

namespace zrpc {
class Acceptor {
 public:
  typedef std::function<void(SocketHandle)> NewConnectionCallback;

  Acceptor(EventLoop *loop, const std::string &ip, int16_t port);

  ~Acceptor();

  void SetNewConnectionCallback(const NewConnectionCallback &&cb) {
    new_connection_callback_ = std::move(cb);
  }

  bool Getlistenning() const { return listenning_; }

  bool Listen();

  void StopListening();

  void HandleRead();

 private:
  Acceptor(const Acceptor &);

  void operator=(const Acceptor &);

  EventLoop *loop_;
  Channel channel_;
  SocketHandle sockfd_;

  NewConnectionCallback new_connection_callback_;
  int idlefd_{-1};
  bool listenning_;
};

}  // namespace zrpc
