#pragma once

#include <functional>
#include <memory>
#include <string>

#include <google/protobuf/service.h>

#include "zrpc/base/timer.h"
#include "zrpc/net/event_loop.h"
#include "zrpc/grpc/rpc_circuit_breaker.h"
#include "zrpc/grpc/rpc_client_channel.h"
#include "zrpc/grpc/rpc_connection_pool.h"
#include "zrpc/grpc/rpc_endpoint.h"
#include "zrpc/grpc/rpc_metrics.h"
#include "zrpc/grpc/rpc_retry_policy.h"
#include "zrpc/grpc/rpc_service_discovery.h"

namespace zrpc {

struct RpcClientOptions {
  int pool_size = 4;
  RpcRetryPolicy retry;
  CircuitBreakerOptions breaker;
  double default_timeout_seconds = 5.0;
  double discovery_refresh_seconds = 0.0;
};

class RpcClient {
 public:
  using ConnectionCallback =
      std::function<void(const std::shared_ptr<TcpConnection>&)>;

  RpcClient(EventLoop* loop, const std::string& ip, uint16_t port,
            const RpcClientOptions& options = {});
  RpcClient(EventLoop* loop, std::shared_ptr<ServiceDiscovery> discovery,
            const RpcClientOptions& options = {});

  ~RpcClient();

  std::shared_ptr<RpcClientChannel> client_channel() const {
    return client_channel_;
  }
  ::google::protobuf::RpcChannel* channel() const {
    return client_channel_.get();
  }
  EventLoop* loop() const { return loop_; }

  RpcMetricsSnapshot GetMetrics() const;
  std::string MetricsString() const;

  void SetConnectionCallback(ConnectionCallback cb);
  void EnableRetry();
  void Connect(bool wait = false);
  void Shutdown();

 private:
  void Init(const RpcClientOptions& options);

  EventLoop* loop_;
  RpcClientOptions options_;
  std::shared_ptr<RpcMetrics> metrics_;
  std::shared_ptr<RpcConnectionPool> pool_;
  std::shared_ptr<RpcClientChannel> client_channel_;
  std::shared_ptr<ServiceDiscovery> discovery_;
  std::shared_ptr<Timer> discovery_timer_;
  ConnectionCallback user_cb_;
};

}  // namespace zrpc
