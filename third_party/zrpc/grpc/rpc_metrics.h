#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace zrpc {

struct RpcMetricsSnapshot {
  uint64_t requests = 0;
  uint64_t successes = 0;
  uint64_t failures = 0;
  uint64_t timeouts = 0;
  uint64_t rejected = 0;
  uint64_t server_requests = 0;
  uint64_t server_errors = 0;
  double avg_latency_us = 0.0;
  double max_latency_us = 0.0;
};

class RpcMetrics {
 public:
  void RecordRequest();
  void RecordSuccess(double latency_us);
  void RecordFailure(bool timeout);
  void RecordRejected();
  void RecordServerRequest();
  void RecordServerError();

  RpcMetricsSnapshot Snapshot() const;

  std::string ToString() const;

 private:
  void UpdateLatency(double latency_us);

  std::atomic<uint64_t> requests_{0};
  std::atomic<uint64_t> successes_{0};
  std::atomic<uint64_t> failures_{0};
  std::atomic<uint64_t> timeouts_{0};
  std::atomic<uint64_t> rejected_{0};
  std::atomic<uint64_t> server_requests_{0};
  std::atomic<uint64_t> server_errors_{0};

  mutable std::mutex latency_mutex_;
  uint64_t latency_samples_ = 0;
  double latency_sum_us_ = 0.0;
  double max_latency_us_ = 0.0;
};

}  // namespace zrpc
