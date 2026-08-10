#pragma once

#include <functional>
#include <string>
#include <utility>

#include "zrpc/rpc/message.h"

namespace zrpc {
namespace rpc {

class Reply {
 public:
  static Reply Ok(std::string body) {
    Reply reply;
    reply.ok_ = true;
    reply.body_ = std::move(body);
    return reply;
  }

  static Reply Error(ErrorCode code, std::string message) {
    Reply reply;
    reply.ok_ = false;
    reply.code_ = code;
    reply.error_ = std::move(message);
    return reply;
  }

  bool ok() const { return ok_; }
  const std::string& body() const { return body_; }
  ErrorCode code() const { return code_; }
  const std::string& error() const { return error_; }

 private:
  bool ok_ = false;
  std::string body_;
  ErrorCode code_ = ErrorCode::kOk;
  std::string error_;
};

using AsyncCallback = std::function<void(const Reply& reply)>;
using Handler = std::function<Reply(const std::string& request)>;

}  // namespace rpc
}  // namespace zrpc
