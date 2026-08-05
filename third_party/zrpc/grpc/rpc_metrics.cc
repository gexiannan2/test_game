#include "zrpc/grpc/rpc_metrics.h"

#include <cmath>
#include <sstream>

namespace zrpc {

void RpcMetrics::RecordRequest() { requests_.fetch_add(1, std::memory_order_relaxed); }

void RpcMetrics::RecordSuccess(double latency_us) {
  successes_.fetch_add(1, std::memory_order_relaxed);
  UpdateLatency(latency_us);
}

void RpcMetrics::RecordFailure(bool timeout) {
  failures_.fetch_add(1, std::memory_order_relaxed);
  if (timeout) {
    timeouts_.fetch_add(1, std::memory_order_relaxed);
  }
}

void RpcMetrics::RecordRejected() {
  rejected_.fetch_add(1, std::memory_order_relaxed);
}

void RpcMetrics::RecordServerRequest() {
  server_requests_.fetch_add(1, std::memory_order_relaxed);
}

void RpcMetrics::RecordServerError() {
  server_errors_.fetch_add(1, std::memory_order_relaxed);
}

void RpcMetrics::UpdateLatency(double latency_us) {
  if (!std::isfinite(latency_us) || latency_us < 0.0) {
    return;
  }
  std::lock_guard<std::mutex> lk(latency_mutex_);
  ++latency_samples_;
  latency_sum_us_ += latency_us;
  if (latency_us > max_latency_us_) {
    max_latency_us_ = latency_us;
  }
}

RpcMetricsSnapshot RpcMetrics::Snapshot() const {
  RpcMetricsSnapshot snap;
  snap.requests = requests_.load(std::memory_order_relaxed);
  snap.successes = successes_.load(std::memory_order_relaxed);
  snap.failures = failures_.load(std::memory_order_relaxed);
  snap.timeouts = timeouts_.load(std::memory_order_relaxed);
  snap.rejected = rejected_.load(std::memory_order_relaxed);
  snap.server_requests = server_requests_.load(std::memory_order_relaxed);
  snap.server_errors = server_errors_.load(std::memory_order_relaxed);

  std::lock_guard<std::mutex> lk(latency_mutex_);
  if (latency_samples_ > 0) {
    snap.avg_latency_us = latency_sum_us_ / static_cast<double>(latency_samples_);
  }
  snap.max_latency_us = max_latency_us_;
  return snap;
}

std::string RpcMetrics::ToString() const {
  const RpcMetricsSnapshot snap = Snapshot();
  std::ostringstream oss;
  oss << "rpc_metrics{req=" << snap.requests << ",ok=" << snap.successes
      << ",fail=" << snap.failures << ",timeout=" << snap.timeouts
      << ",rejected=" << snap.rejected << ",srv_req=" << snap.server_requests
      << ",srv_err=" << snap.server_errors
      << ",avg_us=" << snap.avg_latency_us << ",max_us=" << snap.max_latency_us
      << "}";
  return oss.str();
}

}  // namespace zrpc
