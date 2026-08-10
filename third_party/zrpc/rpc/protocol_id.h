#pragma once

#include <cstdint>
#include <string>

namespace zrpc {
namespace rpc {

using ProtocolId = uint32_t;

constexpr const char* kProtocolService = "zrpc";

inline std::string ProtocolMethodName(ProtocolId id) {
  return std::to_string(id);
}

inline std::string HandlerKeyForProtocol(ProtocolId id) {
  return std::string(kProtocolService) + "." + ProtocolMethodName(id);
}

}  // namespace rpc
}  // namespace zrpc
