#pragma once

#include <google/protobuf/service.h>
#include <mutex>
#include <string>

namespace zrpc {

class RpcController : public ::google::protobuf::RpcController {
 public:
  RpcController() = default;

  void Reset() override;
  bool Failed() const override;
  std::string ErrorText() const override;
  void StartCancel() override;
  void SetFailed(const std::string& reason) override;
  bool IsCanceled() const override;
  void NotifyOnCancel(::google::protobuf::Closure* callback) override;

  void SetErrorCode(int code);
  int ErrorCode() const;

  // Timeout for the next RPC call. 0 means no timeout.
  void SetTimeout(double seconds);
  double TimeoutSeconds() const;

 private:
  mutable std::mutex mutex_;
  bool failed_ = false;
  bool canceled_ = false;
  int error_code_ = 0;
  double timeout_seconds_ = 0.0;
  std::string reason_;
  ::google::protobuf::Closure* cancel_callback_ = nullptr;
};

}  // namespace zrpc
