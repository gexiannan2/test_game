#pragma once

#include <cstdint>
#include <string>

namespace zrpc {
namespace rpc {

enum class MessageType : uint8_t {
  kRequest = 0,
  kResponse = 1,
};

enum class ErrorCode : uint16_t {
  kOk = 0,
  kWrongProto = 1,
  kNoService = 2,
  kNoMethod = 3,
  kInvalidRequest = 4,
  kInvalidResponse = 5,
  kTimeout = 6,
  kTransport = 7,
  kRejected = 8,
};

struct Message {
  MessageType type = MessageType::kRequest;
  int64_t id = 0;
  ErrorCode error = ErrorCode::kOk;
  std::string service;
  std::string method;
  std::string body;
};

const char* ErrorCodeName(ErrorCode code);

}  // namespace rpc
}  // namespace zrpc
