#pragma once

#include <charconv>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace zrpc {

struct RpcEndpoint {
  std::string ip;
  uint16_t port = 0;

  RpcEndpoint() = default;
  RpcEndpoint(std::string ip_arg, uint16_t port_arg)
      : ip(std::move(ip_arg)), port(port_arg) {}

  std::string ToString() const {
    if (ip.find(':') != std::string::npos &&
        !(ip.size() >= 2 && ip.front() == '[' && ip.back() == ']')) {
      return "[" + ip + "]:" + std::to_string(port);
    }
    return ip + ":" + std::to_string(port);
  }

  bool operator==(const RpcEndpoint& other) const {
    return ip == other.ip && port == other.port;
  }

  bool operator!=(const RpcEndpoint& other) const { return !(*this == other); }
};

inline bool ParseEndpoint(const std::string& ip_port, RpcEndpoint* out) {
  if (out == nullptr || ip_port.empty()) {
    return false;
  }
  const size_t pos = ip_port.rfind(':');
  if (pos == std::string::npos || pos == 0 || pos + 1 >= ip_port.size()) {
    return false;
  }

  std::string ip = ip_port.substr(0, pos);
  if (ip.front() == '[') {
    if (ip.size() < 3 || ip.back() != ']') {
      return false;
    }
    ip = ip.substr(1, ip.size() - 2);
  } else if (ip.find(']') != std::string::npos ||
             ip.find('[') != std::string::npos) {
    return false;
  }

  uint32_t port = 0;
  const char* first = ip_port.data() + pos + 1;
  const char* last = ip_port.data() + ip_port.size();
  const auto parsed = std::from_chars(first, last, port);
  if (parsed.ec != std::errc() || parsed.ptr != last || port == 0 ||
      port > std::numeric_limits<uint16_t>::max()) {
    return false;
  }

  out->ip = std::move(ip);
  out->port = static_cast<uint16_t>(port);
  return true;
}

}  // namespace zrpc
