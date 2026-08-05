#include "zrpc/grpc/rpc_client_channel.h"

#include <google/protobuf/stubs/callback.h>

#include <cmath>
#include <exception>
#include <stdexcept>

#include "zrpc/base/logger.h"
#include "zrpc/grpc/rpc_controller.h"

namespace zrpc {

namespace {

double ElapsedUs(const std::chrono::steady_clock::time_point& start) {
  return std::chrono::duration<double, std::micro>(
             std::chrono::steady_clock::now() - start)
      .count();
}

bool IsTimeout(const ::google::protobuf::RpcController* controller) {
  const auto* ctrl = dynamic_cast<const RpcController*>(controller);
  return ctrl != nullptr &&
         ctrl->ErrorCode() == static_cast<int>(TIMEOUT);
}

void RunClosureSafely(::google::protobuf::Closure* done) {
  if (done == nullptr) {
    return;
  }
  try {
    done->Run();
  } catch (const std::exception& ex) {
    LOG_WARN << "rpc client callback threw: " << ex.what();
  } catch (...) {
    LOG_WARN << "rpc client callback threw an unknown exception";
  }
}

}  // namespace

void RpcClientChannel::OnAsyncDone(RpcClientAsyncState* state) {
  state->self->FinishAsync(state);
}

RpcClientChannel::RpcClientChannel(std::shared_ptr<RpcConnectionPool> pool,
                                   std::shared_ptr<RpcMetrics> metrics,
                                   RpcRetryPolicy retry_policy,
                                   CircuitBreakerOptions breaker_options,
                                   double default_timeout_seconds)
    : pool_(std::move(pool)),
      metrics_(std::move(metrics)),
      retry_policy_(retry_policy),
      breaker_(breaker_options),
      default_timeout_seconds_(
          std::isfinite(default_timeout_seconds) &&
                  default_timeout_seconds > 0.0
              ? default_timeout_seconds
              : 5.0) {
  if (!pool_) {
    throw std::invalid_argument(
        "RpcClientChannel requires a connection pool");
  }
}

void RpcClientChannel::FinishAsync(RpcClientAsyncState* state) {
  std::unique_ptr<RpcClientAsyncState> guard(state);
  if (state->controller != nullptr && state->controller->Failed()) {
    breaker_.RecordFailure();
    if (metrics_) {
      metrics_->RecordFailure(IsTimeout(state->controller));
    }
  } else {
    breaker_.RecordSuccess();
    if (metrics_) {
      metrics_->RecordSuccess(ElapsedUs(state->start));
    }
  }
  pool_->Release(state->channel);
  RunClosureSafely(state->done);
}

void RpcClientChannel::CallOnce(
    const ::google::protobuf::MethodDescriptor* method,
    ::google::protobuf::RpcController* controller,
    const ::google::protobuf::Message* request,
    ::google::protobuf::Message* response) {
  RpcChannelPtr channel = pool_->Acquire();
  if (!channel) {
    if (controller != nullptr) {
      controller->SetFailed("no rpc connection available");
      if (auto* ctrl = dynamic_cast<RpcController*>(controller)) {
        ctrl->SetErrorCode(static_cast<int>(TIMEOUT));
      }
    }
    return;
  }
  channel->CallMethod(method, controller, request, response, nullptr);
  pool_->Release(channel);
}

void RpcClientChannel::CallMethod(
    const ::google::protobuf::MethodDescriptor* method,
    ::google::protobuf::RpcController* controller,
    const ::google::protobuf::Message* request,
    ::google::protobuf::Message* response,
    ::google::protobuf::Closure* done) {
  std::shared_ptr<RpcController> owned_controller;
  if (controller == nullptr) {
    owned_controller = std::make_shared<RpcController>();
    controller = owned_controller.get();
  }

  if (metrics_) {
    metrics_->RecordRequest();
  }

  if (controller != nullptr) {
    controller->Reset();
    if (auto* ctrl = dynamic_cast<RpcController*>(controller);
        ctrl != nullptr && ctrl->TimeoutSeconds() <= 0.0) {
      ctrl->SetTimeout(default_timeout_seconds_);
    }
  }

  if (!breaker_.AllowRequest()) {
    if (controller != nullptr) {
      controller->SetFailed("circuit breaker open");
      if (auto* ctrl = dynamic_cast<RpcController*>(controller)) {
        ctrl->SetErrorCode(static_cast<int>(TIMEOUT));
      }
    }
    if (metrics_) {
      metrics_->RecordRejected();
    }
    RunClosureSafely(done);
    return;
  }

  const auto start = std::chrono::steady_clock::now();
  const bool sync = (done == nullptr);

  if (sync) {
    uint32_t attempt = 0;
    while (true) {
      if (controller != nullptr) {
        controller->Reset();
      }
      CallOnce(method, controller, request, response);
      if (controller == nullptr || !controller->Failed()) {
        breaker_.RecordSuccess();
        if (metrics_) {
          metrics_->RecordSuccess(ElapsedUs(start));
        }
        return;
      }
      if (!retry_policy_.ShouldRetry(controller, attempt)) {
        break;
      }
      retry_policy_.SleepBeforeRetry(++attempt);
    }

    breaker_.RecordFailure();
    if (metrics_) {
      metrics_->RecordFailure(IsTimeout(controller));
    }
    return;
  }

  RpcChannelPtr channel = pool_->Acquire();
  if (!channel) {
    if (controller != nullptr) {
      controller->SetFailed("no rpc connection available");
    }
    breaker_.RecordFailure();
    if (metrics_) {
      metrics_->RecordFailure(true);
    }
    RunClosureSafely(done);
    return;
  }

  std::shared_ptr<RpcClientChannel> self = weak_from_this().lock();
  if (!self) {
    pool_->Release(channel);
    if (controller != nullptr) {
      controller->SetFailed(
          "async rpc requires a shared RpcClientChannel instance");
    }
    breaker_.RecordFailure();
    if (metrics_) {
      metrics_->RecordFailure(false);
    }
    RunClosureSafely(done);
    return;
  }

  auto* state = new RpcClientAsyncState();
  state->self = std::move(self);
  state->channel = channel;
  state->owned_controller = std::move(owned_controller);
  state->controller = controller;
  state->done = done;
  state->start = start;
  channel->CallMethod(method, controller, request, response,
                      google::protobuf::NewCallback(&RpcClientChannel::OnAsyncDone,
                                                    state));
}

}  // namespace zrpc
