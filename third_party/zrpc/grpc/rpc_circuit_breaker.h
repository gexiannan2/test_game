#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

namespace zrpc {

struct CircuitBreakerOptions {
  uint32_t failure_threshold = 5;
  double open_seconds = 30.0;
  uint32_t half_open_max_calls = 3;
};

class RpcCircuitBreaker {
 public:
  explicit RpcCircuitBreaker(CircuitBreakerOptions options = {});

  bool AllowRequest();
  void RecordSuccess();
  void RecordFailure();

  bool IsOpen() const;

 private:
  enum class State { kClosed, kOpen, kHalfOpen };

  using Clock = std::chrono::steady_clock;

  CircuitBreakerOptions options_;
  mutable std::mutex mutex_;
  State state_ = State::kClosed;
  uint32_t consecutive_failures_ = 0;
  uint32_t half_open_calls_ = 0;
  Clock::time_point open_until_{};
};

}  // namespace zrpc
