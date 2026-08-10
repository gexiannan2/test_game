#pragma once

#include <google/protobuf/service.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>

#include "zrpc/grpc/rpc.pb.h"
#include "zrpc/grpc/rpc_controller.h"

namespace zrpc {

struct RpcRetryPolicy {
  uint32_t max_retries = 2;
  double base_backoff_seconds = 0.05;
  double max_backoff_seconds = 1.0;
  bool retry_on_timeout = true;

  bool ShouldRetry(const ::google::protobuf::RpcController* controller,
                   uint32_t attempt) const {
    if (attempt >= max_retries || controller == nullptr || !controller->Failed()) {
      return false;
    }
    const auto* ctrl = dynamic_cast<const RpcController*>(controller);
    if (ctrl == nullptr) {
      return true;
    }
    if (ctrl->ErrorCode() == static_cast<int>(TIMEOUT)) {
      return retry_on_timeout;
    }
    return false;
  }

  void SleepBeforeRetry(uint32_t attempt) const {
    if (attempt == 0 || !std::isfinite(base_backoff_seconds) ||
        !std::isfinite(max_backoff_seconds) ||
        base_backoff_seconds <= 0.0 || max_backoff_seconds <= 0.0) {
      return;
    }
    const uint32_t exponent = (std::min)(attempt - 1, uint32_t{62});
    const double backoff =
        (std::min)(max_backoff_seconds,
                   base_backoff_seconds *
                       std::pow(2.0, static_cast<double>(exponent)));
    std::this_thread::sleep_for(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::duration<double>(backoff)));
  }
};

}  // namespace zrpc
