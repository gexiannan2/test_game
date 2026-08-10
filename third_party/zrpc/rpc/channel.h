#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "zrpc/grpc/rpc_metrics.h"
#include "zrpc/rpc/codec.h"
#include "zrpc/rpc/context.h"
#include "zrpc/rpc/message.h"
#include "zrpc/rpc/worker_pool.h"
#include "zrpc/net/tcp_connection.h"

namespace zrpc {

class EventLoop;
class Timer;

namespace rpc {

using RpcHandler = std::function<void(Context*, const std::string& request,
                                       std::string* response)>;
using DoneCallback = std::function<void(Context*, const std::string& response)>;

class Channel : public std::enable_shared_from_this<Channel> {
 public:
  Channel();
  explicit Channel(const std::shared_ptr<TcpConnection>& conn);
  ~Channel();

  void SetConnection(const std::shared_ptr<TcpConnection>& conn);
  void OnDisconnect();
  void PrepareShutdown();
  bool Connected() const;

  void SetHandlers(const std::map<std::string, RpcHandler>* handlers) {
    handlers_.store(handlers, std::memory_order_release);
  }
  void SetMetrics(const std::shared_ptr<RpcMetrics>& metrics) {
    std::atomic_store_explicit(&metrics_, metrics, std::memory_order_release);
  }
  void SetWorkerPool(const std::shared_ptr<WorkerPool>& worker_pool) {
    worker_pool_ = worker_pool;
  }

  void Call(Context* ctx, const std::string& service, const std::string& method,
            const std::string& request, DoneCallback done);
  bool CallSync(Context* ctx, const std::string& service,
                const std::string& method, const std::string& request,
                std::string* response);

  void OnMessage(const std::shared_ptr<TcpConnection>& conn, Buffer* buf);

 private:
  struct OutstandingCall {
    Context* ctx = nullptr;
    DoneCallback done;
    bool sync = false;
    double start_us = 0.0;
    std::shared_ptr<Timer> timeout_timer;
    // 映射删除早于结果写入，独立标记用于正确发布同步调用结果。
    std::shared_ptr<std::atomic<bool>> completed;
  };

  void OnRpcMessage(const std::shared_ptr<TcpConnection>& conn,
                    const Message& message);
  bool RemoveOutstanding(int64_t id, OutstandingCall* out);
  void FailOutstanding(int64_t id, const std::string& reason, ErrorCode code);
  void FailAllOutstanding(const std::string& reason, ErrorCode code);
  void CompleteResponse(const OutstandingCall& out, const Message& message);
  void WaitSyncResponse(
      int64_t id, double timeout_sec,
      const std::shared_ptr<std::atomic<bool>>& completed);
  void OnCallTimeout(int64_t id);
  void ScheduleTimeout(int64_t id, double timeout_sec);
  static void SetContextFailed(Context* ctx, const std::string& reason,
                               ErrorCode code);
  void DispatchRequest(const std::shared_ptr<TcpConnection>& conn,
                       const Message& message, const RpcHandler& handler);
  void SendResponse(const std::shared_ptr<TcpConnection>& conn,
                    Message response, double start_us, bool failed);

  std::shared_ptr<TcpConnection> ConnectionSnapshot() const;
  std::shared_ptr<RpcMetrics> MetricsSnapshot() const;

  Codec codec_;
  std::shared_ptr<TcpConnection> conn_;
  std::atomic<int64_t> id_{1};
  std::atomic<bool> disconnected_{false};
  std::atomic<bool> closing_{false};
  std::atomic<int32_t> server_inflight_{0};

  mutable std::mutex mutex_;
  std::condition_variable sync_cv_;
  std::map<int64_t, OutstandingCall> outstandings_;

  std::atomic<const std::map<std::string, RpcHandler>*> handlers_{nullptr};
  std::shared_ptr<RpcMetrics> metrics_;
  std::shared_ptr<WorkerPool> worker_pool_;
};

using ChannelPtr = std::shared_ptr<Channel>;

}  // namespace rpc
}  // namespace zrpc
