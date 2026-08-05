#pragma once

#include <google/protobuf/service.h>

#include <chrono>
#include <memory>

#include "zrpc/grpc/rpc_circuit_breaker.h"
#include "zrpc/grpc/rpc_connection_pool.h"
#include "zrpc/grpc/rpc_metrics.h"
#include "zrpc/grpc/rpc_retry_policy.h"

namespace zrpc {

class RpcController;

struct RpcClientAsyncState {
  std::shared_ptr<class RpcClientChannel> self;
  RpcChannelPtr channel;
  std::shared_ptr<RpcController> owned_controller;
  ::google::protobuf::RpcController* controller = nullptr;
  ::google::protobuf::Closure* done = nullptr;
  std::chrono::steady_clock::time_point start;
};

class RpcClientChannel : public ::google::protobuf::RpcChannel,
                         public std::enable_shared_from_this<RpcClientChannel> {
 public:
  RpcClientChannel(std::shared_ptr<RpcConnectionPool> pool,
                   std::shared_ptr<RpcMetrics> metrics,
                   RpcRetryPolicy retry_policy = {},
                   CircuitBreakerOptions breaker_options = {},
                   double default_timeout_seconds = 5.0);

  void CallMethod(const ::google::protobuf::MethodDescriptor* method,
                  ::google::protobuf::RpcController* controller,
                  const ::google::protobuf::Message* request,
                  ::google::protobuf::Message* response,
                  ::google::protobuf::Closure* done) override;

  std::shared_ptr<RpcMetrics> metrics() const { return metrics_; }

 private:
  static void OnAsyncDone(RpcClientAsyncState* state);
  void FinishAsync(RpcClientAsyncState* state);

  void CallOnce(const ::google::protobuf::MethodDescriptor* method,
                ::google::protobuf::RpcController* controller,
                const ::google::protobuf::Message* request,
                ::google::protobuf::Message* response);

  std::shared_ptr<RpcConnectionPool> pool_;
  std::shared_ptr<RpcMetrics> metrics_;
  RpcRetryPolicy retry_policy_;
  RpcCircuitBreaker breaker_;
  double default_timeout_seconds_;
};

}  // namespace zrpc
