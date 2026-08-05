#include "tcp_server.h"

#include <exception>
#include <stdexcept>
#include <utility>

#include "zrpc/base/logger.h"
#include "tcp_connection.h"
namespace zrpc {
namespace {
EventLoop* RequireEventLoop(EventLoop* loop) {
  if (loop == nullptr) {
    throw std::invalid_argument("TcpServer event loop must not be null");
  }
  return loop;
}
}  // namespace

TcpServer::TcpServer(EventLoop *loop, const std::string &ip, int16_t port,
                     const std::any &context)
    : loop_(RequireEventLoop(loop)),
      callback_state_(std::make_shared<CallbackState>()),
      acceptor_(new Acceptor(loop_, ip, port)),
      thread_pool_(new ThreadPool(loop_)),
      context_(context) {
  callback_state_->owner = this;
  std::weak_ptr<CallbackState> weak_state = callback_state_;
  acceptor_->SetNewConnectionCallback(
      [weak_state](SocketHandle sockfd) {
        AcceptCallback accept_callback;
        std::shared_ptr<TcpConnection> connection;
        auto state = weak_state.lock();
        if (!state) {
          socket::Close(sockfd);
          return;
        }
        {
          std::lock_guard<std::mutex> lock(state->mutex);
          if (!state->owner) {
            socket::Close(sockfd);
            return;
          }
          accept_callback = state->owner->accept_callback_;
          connection = state->owner->NewConnection(sockfd);
        }
        if (connection) {
          connection->GetLoop()->RunInLoop(
              std::bind(&TcpConnection::ConnectEstablished, connection));
        }
        if (accept_callback) {
          try {
            accept_callback(sockfd);
          } catch (const std::exception& ex) {
            LOG_WARN << "TcpServer accept callback threw: " << ex.what();
            if (connection) {
              connection->GetLoop()->QueueInLoop(
                  std::bind(&TcpConnection::ForceCloseInLoop, connection));
            }
          } catch (...) {
            LOG_WARN << "TcpServer accept callback threw";
            if (connection) {
              connection->GetLoop()->QueueInLoop(
                  std::bind(&TcpConnection::ForceCloseInLoop, connection));
            }
          }
        }
      });
}

TcpServer::~TcpServer() {
  {
    std::lock_guard<std::mutex> lock(callback_state_->mutex);
    callback_state_->owner = nullptr;
  }
  loop_->AssertInLoopThread();
  for (auto &it : connections_) {
    std::shared_ptr<TcpConnection> conn = it.second;
    it.second.reset();
    conn->GetLoop()->RunInLoop(
        std::bind(&TcpConnection::ConnectDestroyed, conn));
    conn.reset();
  }
}

std::shared_ptr<TcpConnection> TcpServer::NewConnection(
    SocketHandle sockfd) {
  loop_->AssertInLoopThread();

  EventLoop *loop = thread_pool_->GetNextLoop();
  std::shared_ptr<TcpConnection> conn(
      new TcpConnection(loop, sockfd, context_));
  connections_[sockfd] = conn;
  conn->SetConnectionCallback(connection_callback_);
  conn->SetMessageCallback(message_callback_);
  conn->SetWriteCompleteCallback(write_complete_callback_);
  std::weak_ptr<CallbackState> weak_state = callback_state_;
  conn->SetCloseCallback(
      [weak_state](const std::shared_ptr<TcpConnection>& closed) {
        auto state = weak_state.lock();
        if (!state) {
          closed->GetLoop()->QueueInLoop(
              std::bind(&TcpConnection::ConnectDestroyed, closed));
          return;
        }
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->owner) {
          state->owner->RemoveConnection(closed);
        } else {
          closed->GetLoop()->QueueInLoop(
              std::bind(&TcpConnection::ConnectDestroyed, closed));
        }
      });
  return conn;
}

void TcpServer::SetThreadNum(int16_t num_threads) {
  thread_pool_->SetThreadNum(num_threads);
}

bool TcpServer::Start() {
  loop_->AssertInLoopThread();
  if (started_) {
    return true;
  }
  try {
    thread_pool_->Start(thread_init_callback_);
    if (!acceptor_->Listen()) {
      thread_pool_->Stop();
      return false;
    }
  } catch (...) {
    if (acceptor_->Getlistenning()) {
      acceptor_->StopListening();
    }
    if (thread_pool_->GetStarted()) {
      thread_pool_->Stop();
    }
    throw;
  }
  started_ = true;
  return true;
}

void TcpServer::Stop(std::function<void()> cb) {
  if (loop_->IsInLoopThread()) {
    stop_callback_ = std::move(cb);
    StopInLoop();
    return;
  }
  std::weak_ptr<CallbackState> weak_state = callback_state_;
  loop_->RunInLoop([weak_state, cb = std::move(cb)]() mutable {
    auto state = weak_state.lock();
    if (!state) {
      return;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->owner) {
      state->owner->stop_callback_ = std::move(cb);
      state->owner->StopInLoop();
    }
  });
}

void TcpServer::StopInLoop() {
  loop_->AssertInLoopThread();
  if (acceptor_->Getlistenning()) {
    acceptor_->StopListening();
  }

  for (const auto &it : connections_) {
    const std::shared_ptr<TcpConnection> &conn = it.second;
    EventLoop *io_loop = conn->GetLoop();
    io_loop->QueueInLoop([conn]() {
      if (conn->Connected()) {
        conn->ForceClose();
      }
    });
  }

  if (!connections_.empty()) {
    std::weak_ptr<CallbackState> weak_state = callback_state_;
    loop_->RunAfter(0.01, false, [weak_state]() {
      auto state = weak_state.lock();
      if (!state) {
        return;
      }
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->owner) {
        state->owner->StopInLoop();
      }
    });
    return;
  }

  if (thread_pool_->GetStarted()) {
    thread_pool_->Stop();
  }
  started_ = false;

  if (stop_callback_) {
    auto cb = std::move(stop_callback_);
    stop_callback_ = nullptr;
    loop_->QueueInLoop([cb = std::move(cb)]() {
      try {
        cb();
      } catch (const std::exception& ex) {
        LOG_WARN << "TcpServer stop callback threw: " << ex.what();
      } catch (...) {
        LOG_WARN << "TcpServer stop callback threw";
      }
    });
  }
}

void TcpServer::RemoveConnection(const std::shared_ptr<TcpConnection> &conn) {
  std::weak_ptr<CallbackState> weak_state = callback_state_;
  loop_->QueueInLoop([weak_state, conn]() {
    auto state = weak_state.lock();
    if (!state) {
      conn->GetLoop()->QueueInLoop(
          std::bind(&TcpConnection::ConnectDestroyed, conn));
      return;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->owner) {
      state->owner->RemoveConnectionInLoop(conn);
    } else {
      conn->GetLoop()->QueueInLoop(
          std::bind(&TcpConnection::ConnectDestroyed, conn));
    }
  });
}

void TcpServer::RemoveConnectionInLoop(
    const std::shared_ptr<TcpConnection> &conn) {
  loop_->AssertInLoopThread();
  size_t n = connections_.erase(conn->GetSockfd());
  (void)n;
  assert(n == 1);
  EventLoop *io_loop = conn->GetLoop();
  io_loop->QueueInLoop(std::bind(&TcpConnection::ConnectDestroyed, conn));
}
}  // namespace zrpc
