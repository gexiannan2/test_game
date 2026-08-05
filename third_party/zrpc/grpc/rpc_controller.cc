#include "zrpc/grpc/rpc_controller.h"

#include <cmath>
#include <exception>

#include "zrpc/base/logger.h"

namespace zrpc {
namespace {

void RunClosureSafely(::google::protobuf::Closure* callback) {
  if (callback == nullptr) {
    return;
  }
  try {
    callback->Run();
  } catch (const std::exception& ex) {
    LOG_WARN << "rpc cancellation callback threw: " << ex.what();
  } catch (...) {
    LOG_WARN << "rpc cancellation callback threw an unknown exception";
  }
}

}  // namespace

void RpcController::Reset() {
  std::lock_guard<std::mutex> lk(mutex_);
  failed_ = false;
  canceled_ = false;
  error_code_ = 0;
  reason_.clear();
  cancel_callback_ = nullptr;
}

bool RpcController::Failed() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return failed_;
}

std::string RpcController::ErrorText() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return reason_;
}

void RpcController::StartCancel() {
  ::google::protobuf::Closure* callback = nullptr;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    if (canceled_) {
      return;
    }
    canceled_ = true;
    callback = cancel_callback_;
    cancel_callback_ = nullptr;
  }
  RunClosureSafely(callback);
}

void RpcController::SetFailed(const std::string& reason) {
  std::lock_guard<std::mutex> lk(mutex_);
  failed_ = true;
  reason_ = reason;
}

bool RpcController::IsCanceled() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return canceled_;
}

void RpcController::NotifyOnCancel(::google::protobuf::Closure* callback) {
  bool run_now = false;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    if (canceled_) {
      run_now = true;
    } else {
      cancel_callback_ = callback;
    }
  }
  if (run_now) {
    RunClosureSafely(callback);
  }
}

void RpcController::SetErrorCode(int code) {
  std::lock_guard<std::mutex> lk(mutex_);
  error_code_ = code;
}

int RpcController::ErrorCode() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return error_code_;
}

void RpcController::SetTimeout(double seconds) {
  std::lock_guard<std::mutex> lk(mutex_);
  timeout_seconds_ =
      std::isfinite(seconds) && seconds > 0.0 ? seconds : 0.0;
}

double RpcController::TimeoutSeconds() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return timeout_seconds_;
}

}  // namespace zrpc
