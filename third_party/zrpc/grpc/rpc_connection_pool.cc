#include "zrpc/grpc/rpc_connection_pool.h"

#include <exception>

#include "zrpc/base/logger.h"

namespace zrpc {

RpcConnectionPool::RpcConnectionPool(EventLoop* loop, RpcEndpoint endpoint,
                                     int pool_size)
    : loop_(loop),
      endpoint_(std::move(endpoint)),
      pool_size_(pool_size > 0 ? pool_size : 1) {}

RpcConnectionPool::~RpcConnectionPool() { Shutdown(); }

void RpcConnectionPool::Shutdown() {
  std::vector<std::shared_ptr<Entry>> entries;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    for (const auto& entry : entries_) {
      entry->active.store(false, std::memory_order_release);
    }
    entries.swap(entries_);
  }
  for (const auto& entry : entries) {
    if (entry && entry->channel) {
      entry->channel->PrepareShutdown();
    }
  }
  entries.clear();
}

void RpcConnectionPool::SetConnectionCallback(ConnectionCallback cb) {
  std::lock_guard<std::mutex> lk(callbacks_->mutex);
  callbacks_->callback = std::move(cb);
}

void RpcConnectionPool::EnableRetry() {
  std::lock_guard<std::mutex> lk(mutex_);
  retry_enabled_ = true;
  for (const auto& entry : entries_) {
    if (entry && entry->client) {
      entry->client->EnableRetry();
    }
  }
}

RpcEndpoint RpcConnectionPool::endpoint() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return endpoint_;
}

void RpcConnectionPool::UpdateEndpoint(const RpcEndpoint& endpoint) {
  if (endpoint.ip.empty() || endpoint.port == 0) {
    return;
  }

  std::vector<std::shared_ptr<Entry>> old_entries;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    if (endpoint_ == endpoint) {
      return;
    }
    endpoint_ = endpoint;
    if (!entries_.empty()) {
      for (const auto& entry : entries_) {
        entry->active.store(false, std::memory_order_release);
      }
      old_entries.swap(entries_);
      RebuildLocked();
    }
  }

  for (const auto& entry : old_entries) {
    if (entry && entry->channel) {
      entry->channel->PrepareShutdown();
    }
  }
}

void RpcConnectionPool::RebuildLocked() {
  if (loop_ == nullptr || endpoint_.ip.empty() || endpoint_.port == 0) {
    return;
  }

  for (int i = 0; i < pool_size_; ++i) {
    auto entry = std::make_shared<Entry>();
    entry->channel = std::make_shared<RpcChannel>();
    entry->client = std::make_unique<TcpClient>(loop_, endpoint_.ip,
                                                static_cast<int16_t>(endpoint_.port),
                                                nullptr);
    if (retry_enabled_) {
      entry->client->EnableRetry();
    }

    std::weak_ptr<Entry> weak_entry = entry;
    std::shared_ptr<CallbackState> callbacks = callbacks_;
    entry->client->SetConnectionCallback(
        [weak_entry, callbacks](const std::shared_ptr<TcpConnection>& conn) {
          OnConnection(weak_entry, callbacks, conn);
        });
    entry->client->SetMessageCallback(
        std::bind(&RpcChannel::OnMessage, entry->channel, std::placeholders::_1,
                  std::placeholders::_2));
    entries_.push_back(std::move(entry));
  }
}

void RpcConnectionPool::Connect(bool wait) {
  std::vector<std::shared_ptr<Entry>> entries;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    if (entries_.empty()) {
      RebuildLocked();
    }
    entries = entries_;
  }

  for (const auto& entry : entries) {
    if (entry->active.load(std::memory_order_acquire) && entry->client) {
      entry->client->Connect(wait);
    }
  }
}

void RpcConnectionPool::OnConnection(
    const std::weak_ptr<Entry>& weak_entry,
    const std::shared_ptr<CallbackState>& callbacks,
    const std::shared_ptr<TcpConnection>& conn) {
  std::shared_ptr<Entry> entry = weak_entry.lock();
  if (!entry || !entry->active.load(std::memory_order_acquire)) {
    return;
  }

  if (conn->Connected()) {
    entry->channel->SetConnection(conn);
    conn->SetCloseCallback([channel = entry->channel, weak_entry](
                               const std::shared_ptr<TcpConnection>& c) {
      channel->OnDisconnect();
      if (std::shared_ptr<Entry> current = weak_entry.lock();
          current && current->active.load(std::memory_order_acquire) &&
          current->client) {
        current->client->HandlePeerClose(c);
      }
    });
  } else {
    entry->channel->OnDisconnect();
  }

  ConnectionCallback callback;
  {
    std::lock_guard<std::mutex> lk(callbacks->mutex);
    callback = callbacks->callback;
  }
  if (callback) {
    try {
      callback(conn);
    } catch (const std::exception& ex) {
      LOG_WARN << "rpc connection callback threw: " << ex.what();
    } catch (...) {
      LOG_WARN << "rpc connection callback threw an unknown exception";
    }
  }
}

RpcChannelPtr RpcConnectionPool::Acquire() {
  std::lock_guard<std::mutex> lk(mutex_);
  if (entries_.empty()) {
    return nullptr;
  }

  size_t best = entries_.size();
  size_t best_load = 0;
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (!entries_[i]->active.load(std::memory_order_acquire) ||
        !entries_[i]->channel || !entries_[i]->channel->Connected()) {
      continue;
    }
    const size_t load = entries_[i]->load.load(std::memory_order_relaxed);
    if (best == entries_.size() || load < best_load) {
      best = i;
      best_load = load;
    }
  }

  if (best == entries_.size()) {
    return nullptr;
  }
  entries_[best]->load.fetch_add(1, std::memory_order_relaxed);
  return entries_[best]->channel;
}

void RpcConnectionPool::Release(const RpcChannelPtr& channel) {
  if (!channel) {
    return;
  }
  std::lock_guard<std::mutex> lk(mutex_);
  for (auto& entry : entries_) {
    if (entry->channel == channel) {
      const size_t load = entry->load.load(std::memory_order_relaxed);
      if (load > 0) {
        entry->load.fetch_sub(1, std::memory_order_relaxed);
      }
      return;
    }
  }
}

bool RpcConnectionPool::AnyConnected() const {
  std::lock_guard<std::mutex> lk(mutex_);
  for (const auto& entry : entries_) {
    if (entry->channel && entry->channel->Connected()) {
      return true;
    }
  }
  return false;
}

}  // namespace zrpc
