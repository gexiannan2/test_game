#include "zrpc/rpc/context.h"

namespace zrpc {
namespace rpc {

void Context::SetFailed(const std::string& text, ErrorCode code) {
  failed_ = true;
  error_text_ = text;
  error_code_ = code;
}

void Context::Reset() {
  failed_ = false;
  error_text_.clear();
  error_code_ = ErrorCode::kOk;
  // timeout_seconds_ intentionally preserved across retries.
}

}  // namespace rpc
}  // namespace zrpc
