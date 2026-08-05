#include "zrpc/rpc/server.h"

#include <algorithm>

namespace zrpc {
namespace rpc {
namespace {

std::string HandlerKey(const std::string& service, const std::string& method) {
  return service + "." + method;
}

}  // namespace

Server::Server(EventLoop* loop, const std::string& ip, uint16_t port,
               const ServerOptions& options)
    : options_(options),
      metrics_(std::make_shared<RpcMetrics>()),
      worker_pool_(std::make_shared<WorkerPool>(
          options.worker_threads, options.max_pending_tasks)),
      server_(loop, ip, port, nullptr) {
  server_.SetConnectionCallback(
      std::bind(&Server::OnConnection, this, std::placeholders::_1));
}

Server::~Server() { PrepareShutdown(); }

RpcHandler Server::WrapHandler(Handler handler) {
  return [handler = std::move(handler)](Context* ctx, const std::string& request,
                                        std::string* response) {
    const Reply reply = handler(request);
    if (!reply.ok()) {
      ctx->SetFailed(reply.error(), reply.code());
      return;
    }
    *response = reply.body();
  };
}

void Server::RegisterProtocol(ProtocolId id, Handler handler) {
  Register(kProtocolService, ProtocolMethodName(id), std::move(handler));
}

void Server::Register(const std::string& service, const std::string& method,
                      Handler handler) {
  if (!handler) {
    return;
  }
  std::lock_guard<std::mutex> lk(handlers_mutex_);
  if (started_.load(std::memory_order_acquire) ||
      shutting_down_.load(std::memory_order_acquire)) {
    return;
  }
  handlers_[HandlerKey(service, method)] = WrapHandler(std::move(handler));
}

bool Server::Start() {
  {
    std::lock_guard<std::mutex> lk(handlers_mutex_);
    if (shutting_down_.load(std::memory_order_acquire)) {
      return false;
    }
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true,
                                          std::memory_order_acq_rel)) {
      return false;
    }
  }
  try {
    if (worker_pool_) {
      worker_pool_->Start();
    }
    if (!server_.Start()) {
      if (worker_pool_) {
        worker_pool_->Stop();
      }
      started_.store(false, std::memory_order_release);
      return false;
    }
  } catch (...) {
    if (worker_pool_) {
      worker_pool_->Stop();
    }
    started_.store(false, std::memory_order_release);
    throw;
  }
  return true;
}

void Server::PrepareShutdown() {
  if (shutting_down_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }

  server_.Stop();

  std::vector<std::weak_ptr<Channel>> channels;
  {
    std::lock_guard<std::mutex> lk(channels_mutex_);
    channels.swap(channels_);
  }
  for (auto& weak_channel : channels) {
    if (auto channel = weak_channel.lock()) {
      channel->PrepareShutdown();
    }
  }
  if (worker_pool_) {
    worker_pool_->Stop();
  }
}

RpcMetricsSnapshot Server::GetMetrics() const {
  return metrics_ ? metrics_->Snapshot() : RpcMetricsSnapshot{};
}

std::string Server::MetricsString() const {
  return metrics_ ? metrics_->ToString() : "rpc_metrics{}";
}

void Server::PruneExpiredChannels() {
  std::lock_guard<std::mutex> lk(channels_mutex_);
  channels_.erase(std::remove_if(channels_.begin(), channels_.end(),
                                 [](const std::weak_ptr<Channel>& w) {
                                   return w.expired();
                                 }),
                   channels_.end());
}

void Server::PruneExpiredChannelsLocked() {
  channels_.erase(std::remove_if(channels_.begin(), channels_.end(),
                                 [](const std::weak_ptr<Channel>& w) {
                                   return w.expired();
                                 }),
                   channels_.end());
}

void Server::OnConnection(const std::shared_ptr<TcpConnection>& conn) {
  if (!conn) {
    return;
  }

  if (conn->Connected()) {
    if (shutting_down_.load(std::memory_order_acquire)) {
      conn->Shutdown();
      return;
    }
    ChannelPtr channel = std::make_shared<Channel>(conn);
    channel->SetHandlers(&handlers_);
    channel->SetMetrics(metrics_);
    channel->SetWorkerPool(worker_pool_);
    conn->SetMessageCallback(
        std::bind(&Channel::OnMessage, channel, std::placeholders::_1,
                  std::placeholders::_2));
    conn->SetCloseCallback([channel](const std::shared_ptr<TcpConnection>&) {
      channel->OnDisconnect();
    });
    conn->SetContext(channel);
    {
      std::lock_guard<std::mutex> lk(channels_mutex_);
      PruneExpiredChannelsLocked();
      channels_.push_back(channel);
    }
  } else {
    if (conn->GetContext().has_value()) {
      if (auto channel = std::any_cast<ChannelPtr>(conn->GetContext())) {
        channel->OnDisconnect();
      }
    }
    conn->ResetContext();
    PruneExpiredChannels();
  }
}

}  // namespace rpc
}  // namespace zrpc
