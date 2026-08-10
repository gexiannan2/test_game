#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "zrpc/grpc/rpc_endpoint.h"

namespace zrpc {

class ServiceDiscovery {
 public:
  virtual ~ServiceDiscovery() = default;

  virtual std::vector<RpcEndpoint> Resolve() = 0;
  virtual RpcEndpoint Preferred() = 0;
};

using LeaderProvider =
    std::function<bool(std::string* ip, int* port)>;
using MembersProvider =
    std::function<void(std::vector<RpcEndpoint>* endpoints)>;

class StaticServiceDiscovery : public ServiceDiscovery {
 public:
  explicit StaticServiceDiscovery(std::vector<RpcEndpoint> endpoints);

  void SetEndpoints(std::vector<RpcEndpoint> endpoints);

  std::vector<RpcEndpoint> Resolve() override;
  RpcEndpoint Preferred() override;

 private:
  mutable std::mutex mutex_;
  std::vector<RpcEndpoint> endpoints_;
};

// Resolves raft leader first, then falls back to cluster members.
class RaftServiceDiscovery : public ServiceDiscovery {
 public:
  RaftServiceDiscovery(LeaderProvider leader, MembersProvider members);

  std::vector<RpcEndpoint> Resolve() override;
  RpcEndpoint Preferred() override;

 private:
  LeaderProvider leader_;
  MembersProvider members_;
};

}  // namespace zrpc
