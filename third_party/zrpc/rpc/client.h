#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "zrpc/grpc/rpc_circuit_breaker.h"
#include "zrpc/grpc/rpc_endpoint.h"
#include "zrpc/grpc/rpc_metrics.h"
#include "zrpc/grpc/rpc_service_discovery.h"
#include "zrpc/net/event_loop.h"
#include "zrpc/rpc/connection_pool.h"
#include "zrpc/rpc/options.h"
#include "zrpc/rpc/protocol_id.h"
#include "zrpc/rpc/reply.h"

namespace zrpc {

class Timer;

namespace rpc {

class Stub;

class Client {
 public:
  using ConnectionCallback = ConnectionPool::ConnectionCallback;

  Client(EventLoop* loop, const std::string& ip, uint16_t port,
         const ClientOptions& options = {});
  Client(EventLoop* loop, std::shared_ptr<ServiceDiscovery> discovery,
         const ClientOptions& options = {});
  ~Client();

  Stub StubFor(const std::string& service, const std::string& method);

  Reply Call(ProtocolId id, const std::string& body,
             const CallOptions& options = {});
  void AsyncCall(ProtocolId id, const std::string& body, AsyncCallback callback,
                 const CallOptions& options = {});

  void SetDefaultCallOptions(CallOptions options);
  void SetConnectionCallback(ConnectionCallback cb);
  void EnableRetry();
  void Connect(bool wait = false);
  void Shutdown();

  RpcMetricsSnapshot GetMetrics() const;
  std::string MetricsString() const;

 private:
  friend class Stub;

  struct AsyncRetryState;

  void Init(const ClientOptions& options);
  static void StartAsyncAttempt(
      const std::shared_ptr<AsyncRetryState>& state);
  static void FinishAsyncAttempt(
      const std::shared_ptr<AsyncRetryState>& state, Context* ctx,
      const std::string& body);
  static void CompleteAsync(const std::shared_ptr<AsyncRetryState>& state,
                            const Reply& reply);

  CallOptions MergeOptions(const CallOptions& options) const;
  Reply InvokeSync(const std::string& service, const std::string& method,
                   const std::string& request, const CallOptions& options);
  void InvokeAsync(const std::string& service, const std::string& method,
                   const std::string& request, AsyncCallback callback,
                   const CallOptions& options);
  static Reply MakeReply(Context* ctx, const std::string& body);

  void CallOnce(Context* ctx, const std::string& service,
                const std::string& method, const std::string& request,
                std::string* response);

  EventLoop* loop_;
  ClientOptions options_;
  std::shared_ptr<RpcMetrics> metrics_;
  std::shared_ptr<ConnectionPool> pool_;
  std::shared_ptr<ServiceDiscovery> discovery_;
  std::shared_ptr<Timer> discovery_timer_;
  std::shared_ptr<RpcCircuitBreaker> breaker_;
  std::shared_ptr<std::atomic<bool>> async_enabled_ =
      std::make_shared<std::atomic<bool>>(true);
  ConnectionCallback user_cb_;
};

struct Client::AsyncRetryState {
  EventLoop* loop = nullptr;
  std::shared_ptr<ConnectionPool> pool;
  std::shared_ptr<RpcMetrics> metrics;
  std::shared_ptr<RpcCircuitBreaker> breaker;
  std::shared_ptr<std::atomic<bool>> enabled;
  RetryPolicy retry;
  std::string service;
  std::string method;
  std::string request;
  AsyncCallback callback;
  CallOptions options;
  uint32_t attempt = 0;
  std::shared_ptr<Context> ctx;
  std::atomic<bool> completed{false};
};

class Stub {
 public:
  Stub(Client* client, std::string service, std::string method);

  Reply Call(const std::string& request,
             const CallOptions& options = {}) const;
  void AsyncCall(const std::string& request, AsyncCallback callback,
                 const CallOptions& options = {}) const;

 private:
  Client* client_;
  std::string service_;
  std::string method_;
};

}  // namespace rpc
}  // namespace zrpc
