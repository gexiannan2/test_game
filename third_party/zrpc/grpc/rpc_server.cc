#include <google/protobuf/descriptor.h>
#include <google/protobuf/service.h>

#include <algorithm>
#include <mutex>
#include <vector>

#include "zrpc/grpc/rpc_server.h"

namespace zrpc {

RpcServer::RpcServer(EventLoop* loop, const std::string& ip, uint16_t port)
    : metrics_(std::make_shared<RpcMetrics>()),
      server_(loop, ip, port, nullptr) {
  server_.SetConnectionCallback(
      std::bind(&RpcServer::OnConnection, this, std::placeholders::_1));
}

RpcServer::~RpcServer() { PrepareShutdown(); }

void RpcServer::RegisterService(::google::protobuf::Service* service) {
  if (service == nullptr || service->GetDescriptor() == nullptr) {
    return;
  }
  const ::google::protobuf::ServiceDescriptor* desc = service->GetDescriptor();
  std::lock_guard<std::mutex> lk(services_mutex_);
  if (started_.load(std::memory_order_acquire) ||
      shutting_down_.load(std::memory_order_acquire)) {
    return;
  }
  services_[desc->full_name()] = service;
}

bool RpcServer::Start() {
  {
    std::lock_guard<std::mutex> lk(services_mutex_);
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
    if (server_.Start()) {
      return true;
    }
  } catch (...) {
    started_.store(false, std::memory_order_release);
    throw;
  }
  started_.store(false, std::memory_order_release);
  return false;
}

void RpcServer::PrepareShutdown() {
  if (shutting_down_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  server_.Stop();
  std::vector<std::weak_ptr<RpcChannel>> channels;
  {
    std::lock_guard<std::mutex> lk(channels_mutex_);
    channels.swap(channels_);
  }
  for (auto& weak_channel : channels) {
    if (auto channel = weak_channel.lock()) {
      channel->PrepareShutdown();
    }
  }
  std::lock_guard<std::mutex> lk(services_mutex_);
  services_.clear();
}

RpcMetricsSnapshot RpcServer::GetMetrics() const {
  return metrics_ ? metrics_->Snapshot() : RpcMetricsSnapshot{};
}

std::string RpcServer::MetricsString() const {
  return metrics_ ? metrics_->ToString() : "rpc_metrics{}";
}

void RpcServer::PruneExpiredChannels() {
  std::lock_guard<std::mutex> lk(channels_mutex_);
  PruneExpiredChannelsLocked();
}

void RpcServer::PruneExpiredChannelsLocked() {
  channels_.erase(std::remove_if(channels_.begin(), channels_.end(),
                                 [](const std::weak_ptr<RpcChannel>& w) {
                                   return w.expired();
                                 }),
                   channels_.end());
}

void RpcServer::OnConnection(const std::shared_ptr<TcpConnection>& conn) {
  if (!conn) {
    return;
  }
  if (conn->Connected()) {
    if (shutting_down_.load(std::memory_order_acquire)) {
      conn->Shutdown();
      return;
    }
    RpcChannelPtr channel = std::make_shared<RpcChannel>(conn);
    {
      std::lock_guard<std::mutex> lk(services_mutex_);
      channel->SetServices(&services_);
    }
    channel->SetMetrics(metrics_);
    conn->SetMessageCallback(
        std::bind(&RpcChannel::OnMessage, channel, std::placeholders::_1,
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
      if (auto channel = std::any_cast<RpcChannelPtr>(conn->GetContext())) {
        channel->OnDisconnect();
      }
    }
    conn->ResetContext();
    PruneExpiredChannels();
  }
}

}  // namespace zrpc
