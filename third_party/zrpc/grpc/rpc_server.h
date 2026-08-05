#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "zrpc/net/tcp_connection.h"
#include "zrpc/net/tcp_server.h"
#include "zrpc/grpc/rpc_channel.h"
#include "zrpc/grpc/rpc_metrics.h"

namespace google {
namespace protobuf {
class Service;
}
}  // namespace google

namespace zrpc {

class RpcServer {
 public:
  RpcServer(EventLoop* loop, const std::string& ip, uint16_t port);
  ~RpcServer();

  void SetThreadNum(int num_threads) { server_.SetThreadNum(num_threads); }

  void RegisterService(::google::protobuf::Service* service);
  bool Start();
  void PrepareShutdown();

  RpcMetricsSnapshot GetMetrics() const;
  std::string MetricsString() const;

 private:
  void OnConnection(const std::shared_ptr<TcpConnection>& conn);
  void PruneExpiredChannels();
  void PruneExpiredChannelsLocked();

  std::map<std::string, ::google::protobuf::Service*> services_;
  mutable std::mutex services_mutex_;
  std::shared_ptr<RpcMetrics> metrics_;
  std::mutex channels_mutex_;
  std::vector<std::weak_ptr<RpcChannel>> channels_;
  std::atomic<bool> started_{false};
  std::atomic<bool> shutting_down_{false};
  TcpServer server_;
};

}  // namespace zrpc
