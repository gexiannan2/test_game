#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "zrpc/net/event_loop.h"
#include "zrpc/net/tcp_client.h"
#include "zrpc/grpc/rpc_channel.h"
#include "zrpc/grpc/rpc_endpoint.h"

namespace zrpc {

class RpcConnectionPool {
 public:
  using ConnectionCallback =
      std::function<void(const std::shared_ptr<TcpConnection>&)>;

  RpcConnectionPool(EventLoop* loop, RpcEndpoint endpoint, int pool_size);
  ~RpcConnectionPool();

  void SetConnectionCallback(ConnectionCallback cb);
  void EnableRetry();
  void Connect(bool wait = false);
  void Shutdown();
  void UpdateEndpoint(const RpcEndpoint& endpoint);

  RpcChannelPtr Acquire();
  void Release(const RpcChannelPtr& channel);

  bool AnyConnected() const;
  size_t Size() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return entries_.size();
  }
  RpcEndpoint endpoint() const;

 private:
  struct Entry {
    std::unique_ptr<TcpClient> client;
    RpcChannelPtr channel;
    std::atomic<size_t> load{0};
    std::atomic<bool> active{true};
  };

  struct CallbackState {
    std::mutex mutex;
    ConnectionCallback callback;
  };

  static void OnConnection(const std::weak_ptr<Entry>& weak_entry,
                           const std::shared_ptr<CallbackState>& callbacks,
                           const std::shared_ptr<TcpConnection>& conn);
  void RebuildLocked();

  EventLoop* loop_;
  RpcEndpoint endpoint_;
  int pool_size_;
  bool retry_enabled_ = false;
  std::shared_ptr<CallbackState> callbacks_ =
      std::make_shared<CallbackState>();

  mutable std::mutex mutex_;
  std::vector<std::shared_ptr<Entry>> entries_;
};

}  // namespace zrpc
