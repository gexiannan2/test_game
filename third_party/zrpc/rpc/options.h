#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <thread>

#include "zrpc/grpc/rpc_circuit_breaker.h"
#include "zrpc/rpc/context.h"
#include "zrpc/rpc/message.h"

namespace zrpc {
namespace rpc {

struct CallOptions {
  // 非正值表示沿用客户端默认超时。
  double timeout_seconds = 0.0;
};

struct RetryPolicy {
  uint32_t max_retries = 2;
  double base_backoff_seconds = 0.05;
  double max_backoff_seconds = 1.0;
  bool retry_on_timeout = true;

  bool ShouldRetry(const Context* ctx, uint32_t attempt) const {
    if (attempt >= max_retries || ctx == nullptr || !ctx->Failed()) {
      return false;
    }
    if (ctx->GetErrorCode() == ErrorCode::kTimeout) {
      return retry_on_timeout;
    }
    return ctx->GetErrorCode() != ErrorCode::kRejected;
  }

  void SleepBeforeRetry(uint32_t attempt) const {
    const double backoff = BackoffSeconds(attempt);
    if (backoff <= 0.0) {
      return;
    }
    std::this_thread::sleep_for(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::duration<double>(backoff)));
  }

  double BackoffSeconds(uint32_t attempt) const {
    if (attempt == 0 || !std::isfinite(base_backoff_seconds) ||
        !std::isfinite(max_backoff_seconds) ||
        base_backoff_seconds <= 0.0 || max_backoff_seconds <= 0.0) {
      return 0.0;
    }
    const uint32_t exponent = (std::min)(attempt - 1, uint32_t{62});
    return (std::min)(max_backoff_seconds,
                       base_backoff_seconds *
                           std::pow(2.0, static_cast<double>(exponent)));
  }
};

struct ClientOptions {
  int pool_size = 4;
  CallOptions defaults{5.0};
  RetryPolicy retry;
  CircuitBreakerOptions breaker;
  double discovery_refresh_seconds = 0.0;
};

struct ServerOptions {
  int worker_threads = 4;
  size_t max_pending_tasks = 1024;
};

}  // namespace rpc
}  // namespace zrpc
