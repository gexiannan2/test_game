#include "zrpc/rpc/message.h"

namespace zrpc {
namespace rpc {

const char* ErrorCodeName(ErrorCode code) {
  switch (code) {
    case ErrorCode::kOk:
      return "OK";
    case ErrorCode::kWrongProto:
      return "WRONG_PROTO";
    case ErrorCode::kNoService:
      return "NO_SERVICE";
    case ErrorCode::kNoMethod:
      return "NO_METHOD";
    case ErrorCode::kInvalidRequest:
      return "INVALID_REQUEST";
    case ErrorCode::kInvalidResponse:
      return "INVALID_RESPONSE";
    case ErrorCode::kTimeout:
      return "TIMEOUT";
    case ErrorCode::kTransport:
      return "TRANSPORT";
    case ErrorCode::kRejected:
      return "REJECTED";
    default:
      return "UNKNOWN";
  }
}

}  // namespace rpc
}  // namespace zrpc
