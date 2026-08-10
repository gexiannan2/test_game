#include "zrpc/grpc/rpc_service_discovery.h"

#include <algorithm>
#include <limits>

namespace zrpc {

StaticServiceDiscovery::StaticServiceDiscovery(std::vector<RpcEndpoint> endpoints)
    : endpoints_(std::move(endpoints)) {}

void StaticServiceDiscovery::SetEndpoints(std::vector<RpcEndpoint> endpoints) {
  std::lock_guard<std::mutex> lk(mutex_);
  endpoints_ = std::move(endpoints);
}

std::vector<RpcEndpoint> StaticServiceDiscovery::Resolve() {
  std::lock_guard<std::mutex> lk(mutex_);
  return endpoints_;
}

RpcEndpoint StaticServiceDiscovery::Preferred() {
  std::lock_guard<std::mutex> lk(mutex_);
  if (endpoints_.empty()) {
    return {};
  }
  return endpoints_.front();
}

RaftServiceDiscovery::RaftServiceDiscovery(LeaderProvider leader,
                                           MembersProvider members)
    : leader_(std::move(leader)), members_(std::move(members)) {}

std::vector<RpcEndpoint> RaftServiceDiscovery::Resolve() {
  std::vector<RpcEndpoint> endpoints;
  if (members_) {
    members_(&endpoints);
  }

  std::string ip;
  int port = 0;
  if (leader_ && leader_(&ip, &port) && !ip.empty() && port > 0 &&
      port <= std::numeric_limits<uint16_t>::max()) {
    const RpcEndpoint leader_ep{ip, static_cast<uint16_t>(port)};
    auto it = std::find(endpoints.begin(), endpoints.end(), leader_ep);
    if (it == endpoints.end()) {
      endpoints.insert(endpoints.begin(), leader_ep);
    } else if (it != endpoints.begin()) {
      std::rotate(endpoints.begin(), it, it + 1);
    }
  }
  return endpoints;
}

RpcEndpoint RaftServiceDiscovery::Preferred() {
  std::string ip;
  int port = 0;
  if (leader_ && leader_(&ip, &port) && !ip.empty() && port > 0 &&
      port <= std::numeric_limits<uint16_t>::max()) {
    return RpcEndpoint{ip, static_cast<uint16_t>(port)};
  }

  std::vector<RpcEndpoint> endpoints;
  if (members_) {
    members_(&endpoints);
  }
  if (!endpoints.empty()) {
    return endpoints.front();
  }
  return {};
}

}  // namespace zrpc
