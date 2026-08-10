#include "zrpc/grpc/rpc_circuit_breaker.h"

#include <cmath>

namespace zrpc {

RpcCircuitBreaker::RpcCircuitBreaker(CircuitBreakerOptions options)
    : options_(options) {
  if (options_.failure_threshold == 0) {
    options_.failure_threshold = 1;
  }
  if (!std::isfinite(options_.open_seconds) || options_.open_seconds < 0.0) {
    options_.open_seconds = 30.0;
  }
  if (options_.half_open_max_calls == 0) {
    options_.half_open_max_calls = 1;
  }
}

bool RpcCircuitBreaker::IsOpen() const {
  std::lock_guard<std::mutex> lk(mutex_);
  if (state_ != State::kOpen) {
    return false;
  }
  return Clock::now() < open_until_;
}

bool RpcCircuitBreaker::AllowRequest() {
  std::lock_guard<std::mutex> lk(mutex_);
  const auto now = Clock::now();
  if (state_ == State::kOpen) {
    if (now < open_until_) {
      return false;
    }
    state_ = State::kHalfOpen;
    half_open_calls_ = 0;
  }

  if (state_ == State::kHalfOpen) {
    if (half_open_calls_ >= options_.half_open_max_calls) {
      return false;
    }
    ++half_open_calls_;
  }
  return true;
}

void RpcCircuitBreaker::RecordSuccess() {
  std::lock_guard<std::mutex> lk(mutex_);
  consecutive_failures_ = 0;
  state_ = State::kClosed;
  half_open_calls_ = 0;
}

void RpcCircuitBreaker::RecordFailure() {
  std::lock_guard<std::mutex> lk(mutex_);
  ++consecutive_failures_;
  if (state_ == State::kHalfOpen ||
      consecutive_failures_ >= options_.failure_threshold) {
    state_ = State::kOpen;
    open_until_ = Clock::now() +
                  std::chrono::duration_cast<Clock::duration>(
                      std::chrono::duration<double>(options_.open_seconds));
    half_open_calls_ = 0;
  }
}

}  // namespace zrpc
