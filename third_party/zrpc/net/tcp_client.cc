#include "zrpc/net/tcp_client.h"

#include <exception>

#include "zrpc/base/logger.h"
#include "zrpc/net/tcp_connection.h"

namespace zrpc {
TcpClient::TcpClient(EventLoop *loop, const std::string &ip, int16_t port,
                     const std::any &context)
    : connector_(std::make_shared<Connector>(loop, ip.c_str(), port, false)),
      loop_(loop),
      callback_state_(std::make_shared<CallbackState>()),
      connection_(nullptr),
      context_(context),
      ip_(ip),
      port_(port),
      retry_(false),
      connecting_(true) {
  callback_state_->owner = this;
  std::weak_ptr<CallbackState> weak_state = callback_state_;
  connector_->SetNewConnectionCallback(
      [weak_state](SocketHandle sockfd) {
        auto state = weak_state.lock();
        if (!state) {
          socket::Close(sockfd);
          return;
        }
        std::shared_ptr<TcpConnection> connection;
        try {
          {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->owner) {
              connection = state->owner->NewConnection(sockfd);
            } else {
              socket::Close(sockfd);
            }
          }
        } catch (const std::exception& ex) {
          LOG_WARN << "TcpClient new-connection callback threw: " << ex.what();
          socket::Close(sockfd);
          return;
        } catch (...) {
          LOG_WARN << "TcpClient new-connection callback threw";
          socket::Close(sockfd);
          return;
        }
        if (connection) {
          connection->ConnectEstablished();
          bool keep_connection = false;
          {
            std::lock_guard<std::mutex> lock(state->mutex);
            keep_connection =
                state->owner &&
                state->owner->connecting_.load(std::memory_order_acquire);
          }
          if (!keep_connection) {
            connection->ForceCloseInLoop();
          }
        }
      });
  connector_->SetConnectionErrorCallBack(
      [weak_state]() {
        ConnectionErrorCallback callback;
        auto state = weak_state.lock();
        if (!state) {
          return;
        }
        {
          std::lock_guard<std::mutex> lock(state->mutex);
          if (state->owner) {
            callback = state->owner->connection_error_callBack_;
          }
        }
        if (callback) {
          try {
            callback();
          } catch (const std::exception& ex) {
            LOG_WARN << "TcpClient connection-error callback threw: "
                     << ex.what();
          } catch (...) {
            LOG_WARN << "TcpClient connection-error callback threw";
          }
        }
      });
}

namespace detail {
void RemoveConnection(EventLoop *loop,
                      const std::shared_ptr<TcpConnection> &conn) {
  loop->RunInLoop(std::bind(&TcpConnection::ConnectDestroyed, conn));
}

void RemoveConnector(const std::shared_ptr<Connector> &connector) {
  (void)connector;
}
}  // namespace detail

TcpClient::~TcpClient() {
  {
    std::lock_guard<std::mutex> lock(callback_state_->mutex);
    callback_state_->owner = nullptr;
  }
  std::shared_ptr<TcpConnection> conn;
  bool unique = false;
  {
    std::unique_lock<std::mutex> lk(mutex_);
    unique = connection_.use_count() == 1;
    conn = connection_;
  }

  if (conn) {
    assert(loop_ == conn->GetLoop());
    CloseCallback cb = std::bind(&detail::RemoveConnection, loop_,
                                 std::placeholders::_1);
    loop_->RunInLoop(std::bind(&TcpConnection::SetCloseCallback, conn, cb));
    if (unique) {
      if (loop_->IsInLoopThread()) {
        conn->ForceCloseInLoop();
      } else {
        loop_->QueueInLoop(
            std::bind(&TcpConnection::ForceCloseInLoop, conn));
      }
    }
  } else {
    connector_->Stop();
    loop_->RunAfter(1, false, std::bind(&detail::RemoveConnector, connector_));
  }
}

void TcpClient::EnableRetry() {
  connector_->EnableRetry();
  retry_ = true;
}

void TcpClient::CloseRetry() {
  connector_->CloseRetry();
  retry_ = false;
}

void TcpClient::Connect(bool s) {
  connecting_ = true;
  connector_->Start(s);
}

void TcpClient::DisConnect() {
  connecting_ = false;
  {
    std::unique_lock<std::mutex> lk(mutex_);
    if (connection_) {
      connection_->Shutdown();
    }
  }
}

std::shared_ptr<TcpConnection> TcpClient::GetConnection() {
  std::unique_lock<std::mutex> lk(mutex_);
  return connection_;
}

void TcpClient::Stop() {
  connecting_ = false;
  connector_->Stop();
}

std::shared_ptr<TcpConnection> TcpClient::NewConnection(
    SocketHandle sockfd) {
  loop_->AssertInLoopThread();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (connection_ ||
        !connecting_.load(std::memory_order_acquire)) {
      socket::Close(sockfd);
      return nullptr;
    }
  }
  std::shared_ptr<TcpConnection> conn(
      new TcpConnection(loop_, sockfd, context_));
  conn->SetConnectionCallback(connection_callback_);
  conn->SetMessageCallback(message_callback_);
  conn->SetWriteCompleteCallback(write_complete_callback_);
  std::weak_ptr<CallbackState> weak_state = callback_state_;
  conn->SetCloseCallback([weak_state](
                             const std::shared_ptr<TcpConnection>& closed) {
    auto state = weak_state.lock();
    if (!state) {
      detail::RemoveConnection(closed->GetLoop(), closed);
      return;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->owner) {
      state->owner->RemoveConnection(closed);
    } else {
      detail::RemoveConnection(closed->GetLoop(), closed);
    }
  });

  {
    std::unique_lock<std::mutex> lk(mutex_);
    connection_ = conn;
  }
  return conn;
}

void TcpClient::HandlePeerClose(const std::shared_ptr<TcpConnection>& conn) {
  RemoveConnection(conn);
}

void TcpClient::RemoveConnection(const std::shared_ptr<TcpConnection> &conn) {
  loop_->AssertInLoopThread();
  assert(loop_ == conn->GetLoop());
  {
    std::unique_lock<std::mutex> lk(mutex_);
    if (connection_ != conn) {
      return;
    }
    connection_.reset();
  }

  loop_->QueueInLoop(std::bind(&TcpConnection::ConnectDestroyed, conn));
  if (retry_ && connecting_) {
    auto connector = connector_;
    loop_->QueueInLoop([connector]() { connector->Restart(); });
  }
}
}  // namespace zrpc
