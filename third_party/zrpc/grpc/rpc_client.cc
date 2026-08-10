#include "zrpc/grpc/rpc_client.h"

#include <cmath>
#include <stdexcept>

#include "zrpc/base/timer.h"

namespace zrpc {

RpcClient::RpcClient(EventLoop* loop, const std::string& ip, uint16_t port,
                     const RpcClientOptions& options)
    : loop_(loop) {
  discovery_ = std::make_shared<StaticServiceDiscovery>(
      std::vector<RpcEndpoint>{RpcEndpoint{ip, port}});
  Init(options);
}

RpcClient::RpcClient(EventLoop* loop,
                     std::shared_ptr<ServiceDiscovery> discovery,
                     const RpcClientOptions& options)
    : loop_(loop), discovery_(std::move(discovery)) {
  Init(options);
}

void RpcClient::Init(const RpcClientOptions& options) {
  if (loop_ == nullptr) {
    throw std::invalid_argument("rpc client requires a valid event loop");
  }
  if (!discovery_) {
    throw std::invalid_argument("rpc client requires service discovery");
  }

  options_ = options;
  if (!std::isfinite(options_.default_timeout_seconds) ||
      options_.default_timeout_seconds <= 0.0) {
    options_.default_timeout_seconds = 5.0;
  }
  if (!std::isfinite(options_.discovery_refresh_seconds) ||
      options_.discovery_refresh_seconds < 0.0) {
    options_.discovery_refresh_seconds = 0.0;
  }
  metrics_ = std::make_shared<RpcMetrics>();

  const RpcEndpoint preferred = discovery_->Preferred();
  pool_ = std::make_shared<RpcConnectionPool>(loop_, preferred,
                                              options_.pool_size);
  client_channel_ = std::make_shared<RpcClientChannel>(
      pool_, metrics_, options_.retry, options_.breaker,
      options_.default_timeout_seconds);

  if (options_.discovery_refresh_seconds > 0.0) {
    std::shared_ptr<ServiceDiscovery> discovery = discovery_;
    std::weak_ptr<RpcConnectionPool> weak_pool = pool_;
    discovery_timer_ = loop_->RunAfter(
        options_.discovery_refresh_seconds, true,
        [discovery = std::move(discovery), weak_pool]() {
          std::shared_ptr<RpcConnectionPool> pool = weak_pool.lock();
          if (!pool) {
            return;
          }
          const RpcEndpoint endpoint = discovery->Preferred();
          if (endpoint.ip.empty() || endpoint.port == 0 ||
              pool->endpoint() == endpoint) {
            return;
          }
          pool->UpdateEndpoint(endpoint);
          pool->Connect(false);
        });
  }
}

void RpcClient::SetConnectionCallback(ConnectionCallback cb) {
  user_cb_ = std::move(cb);
  pool_->SetConnectionCallback(user_cb_);
}

void RpcClient::EnableRetry() { pool_->EnableRetry(); }

void RpcClient::Connect(bool wait) { pool_->Connect(wait); }

void RpcClient::Shutdown() {
  if (discovery_timer_ && loop_) {
    loop_->CancelAfter(discovery_timer_);
    discovery_timer_.reset();
  }
  if (pool_) {
    pool_->Shutdown();
  }
}

RpcClient::~RpcClient() { Shutdown(); }

RpcMetricsSnapshot RpcClient::GetMetrics() const {
  return metrics_ ? metrics_->Snapshot() : RpcMetricsSnapshot{};
}

std::string RpcClient::MetricsString() const {
  return metrics_ ? metrics_->ToString() : "rpc_metrics{}";
}

}  // namespace zrpc
