#pragma once

#include <string>

#include "zrpc/rpc/message.h"

namespace zrpc {
namespace rpc {

class Context {
 public:
  bool Failed() const { return failed_; }
  const std::string& ErrorText() const { return error_text_; }
  ErrorCode GetErrorCode() const { return error_code_; }
  double TimeoutSeconds() const { return timeout_seconds_; }

  void SetTimeout(double seconds) { timeout_seconds_ = seconds; }
  void SetFailed(const std::string& text, ErrorCode code);
  void Reset();

 private:
  bool failed_ = false;
  std::string error_text_;
  ErrorCode error_code_ = ErrorCode::kOk;
  double timeout_seconds_ = 0.0;
};

}  // namespace rpc
}  // namespace zrpc
