#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "zrpc/grpc/rpc_metrics.h"
#include "zrpc/net/tcp_connection.h"
#include "zrpc/net/tcp_server.h"
#include "zrpc/rpc/channel.h"
#include "zrpc/rpc/options.h"
#include "zrpc/rpc/protocol_id.h"
#include "zrpc/rpc/reply.h"
#include "zrpc/rpc/worker_pool.h"

namespace zrpc {
namespace rpc {

class Server {
 public:
  Server(EventLoop* loop, const std::string& ip, uint16_t port,
         const ServerOptions& options = {});
  ~Server();

  void SetThreadNum(int num_threads) { server_.SetThreadNum(num_threads); }
  void RegisterProtocol(ProtocolId id, Handler handler);
  void Register(const std::string& service, const std::string& method,
                Handler handler);
  bool Start();
  void PrepareShutdown();

  RpcMetricsSnapshot GetMetrics() const;
  std::string MetricsString() const;

 private:
  void OnConnection(const std::shared_ptr<TcpConnection>& conn);
  static RpcHandler WrapHandler(Handler handler);
  void PruneExpiredChannels();
  void PruneExpiredChannelsLocked();

  ServerOptions options_;
  std::map<std::string, RpcHandler> handlers_;
  std::mutex handlers_mutex_;
  std::shared_ptr<RpcMetrics> metrics_;
  std::shared_ptr<WorkerPool> worker_pool_;
  std::mutex channels_mutex_;
  std::vector<std::weak_ptr<Channel>> channels_;
  std::atomic<bool> started_{false};
  std::atomic<bool> shutting_down_{false};
  TcpServer server_;
};

}  // namespace rpc
}  // namespace zrpc
