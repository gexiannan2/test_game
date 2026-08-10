#include "zrpc/net/connector.h"

#include <cerrno>
#include <stdexcept>

#include "zrpc/net/socket.h"

namespace zrpc {
namespace {
EventLoop* RequireEventLoop(EventLoop* loop) {
  if (loop == nullptr) {
    throw std::invalid_argument("Connector event loop must not be null");
  }
  return loop;
}

const char* RequireIp(const char* ip) {
  if (ip == nullptr || *ip == '\0') {
    throw std::invalid_argument("Connector address must not be empty");
  }
  return ip;
}

int32_t GetConnectError(int32_t ret) {
  if (ret == 0) {
    return 0;
  }
#ifdef _WIN32
  int32_t err = static_cast<int32_t>(::WSAGetLastError());
  if (err == 0) {
    return EINPROGRESS;
  }
  if (err == WSAEWOULDBLOCK) return EINPROGRESS;
  if (err == WSAEISCONN) return EISCONN;
  if (err == WSAEINTR) return EINTR;
  if (err == WSAEADDRINUSE) return EADDRINUSE;
  if (err == WSAEADDRNOTAVAIL) return EADDRNOTAVAIL;
  if (err == WSAECONNREFUSED) return ECONNREFUSED;
  if (err == WSAENETUNREACH) return ENETUNREACH;
  if (err == WSAEACCES) return EACCES;
  if (err == WSAEAFNOSUPPORT) return EAFNOSUPPORT;
  if (err == WSAEALREADY) return EALREADY;
  if (err == WSAEBADF) return EBADF;
  if (err == WSAEFAULT) return EFAULT;
  if (err == WSAENOTSOCK) return ENOTSOCK;
  return err;
#else
  return errno;
#endif
}
}  // namespace

const int Connector::kMaxRetryDelayMs;

Connector::Connector(EventLoop *loop_, const char *ip, int16_t port, bool retry)
    : loop_(RequireEventLoop(loop_)),
      ip_(RequireIp(ip)),
      port_(port),
      connect_(false),
      retry_(retry),
      retry_delay_ms_(kInitRetryDelayMs),
      state_(kDisconnected) {}

Connector::~Connector() {
#ifdef _WIN32
  if (socket::IsValid(connect_sockfd_)) {
    socket::Close(connect_sockfd_);
    connect_sockfd_ = kInvalidSocket;
  }
#endif
  if (channel_) {
    if (loop_->IsInLoopThread()) {
      const SocketHandle sockfd = channel_->Getfd();
      channel_->DisableAll();
      channel_->Remove();
      channel_.reset();
      socket::Close(sockfd);
    } else {
      Channel* ch = channel_.release();
      loop_->QueueInLoop([loop = loop_, ch]() {
        const SocketHandle sockfd = ch->Getfd();
        ch->DisableAll();
        ch->Remove();
        delete ch;
        socket::Close(sockfd);
      });
    }
  }
}

void Connector::Start(bool state) {
  const bool was_connecting =
      connect_.exchange(true, std::memory_order_acq_rel);
  auto self = shared_from_this();
  loop_->RunInLoop([self, state, was_connecting]() {
    if (!was_connecting) {
      self->retry_delay_ms_ = kInitRetryDelayMs;
    }
    self->StartInLoop(state);
  });
}

void Connector::StartInLoop(bool state) {
  loop_->AssertInLoopThread();
  if (state_ == kConnected) {
    SetState(kDisconnected);
  }
  if (state_ == kConnecting) {
    return;
  }
  assert(state_ == kDisconnected);
  if (connect_) {
    Connecting(state);
  } else {
    LOG_WARN << "do not connect_";
  }
}

void Connector::Stop() {
  connect_ = false;
  loop_->QueueInLoop(
      std::bind(&Connector::StopInLoop, shared_from_this()));
}

void Connector::StopInLoop() {
  loop_->AssertInLoopThread();
  if (state_ == kConnecting) {
    SetState(kDisconnected);
#ifdef _WIN32
    if (socket::IsValid(connect_sockfd_)) {
      socket::Close(connect_sockfd_);
      connect_sockfd_ = kInvalidSocket;
    }
#endif
    if (channel_) {
      socket::Close(RemoveAndResetChannel());
    }
  }
}

void Connector::ResetChannel() { channel_.reset(); }

SocketHandle Connector::RemoveAndResetChannel() {
  channel_->DisableAll();
  channel_->Remove();
  SocketHandle sockfd = channel_->Getfd();
  loop_->QueueInLoop(
      std::bind(&Connector::ResetChannel, shared_from_this()));
  return sockfd;
}

void Connector::Connecting(bool state, SocketHandle sockfd) {
  if (state) {
    SetState(kConnected);
    if (connect_) {
      socket::SetSocketNonBlock(sockfd);
      if (new_connection_callback_) {
        new_connection_callback_(sockfd);
      } else {
        socket::Close(sockfd);
      }
    } else {
#ifdef _WIN32
      loop_->ReleaseSocketContext(sockfd);
#endif
      socket::Close(sockfd);
    }
  } else {
#ifdef _WIN32
    connect_sockfd_ = sockfd;
    PollConnect();
#else
    assert(!channel_);
    channel_.reset(new Channel(loop_, sockfd));
    channel_->SetWriteCallback(
        std::bind(&Connector::HandleWrite, shared_from_this()));
    channel_->SetErrorCallback(
        std::bind(&Connector::HandleError, shared_from_this()));
    channel_->EnableWriting();
#endif
  }
}

#ifdef _WIN32
void Connector::PollConnect() {
  loop_->AssertInLoopThread();
  if (state_ != kConnecting || !socket::IsValid(connect_sockfd_)) {
    return;
  }

  fd_set wfds;
  fd_set efds;
  FD_ZERO(&wfds);
  FD_ZERO(&efds);
  FD_SET(static_cast<SOCKET>(connect_sockfd_), &wfds);
  FD_SET(static_cast<SOCKET>(connect_sockfd_), &efds);
  timeval tv = {0, 0};
  int n = ::select(0, nullptr, &wfds, &efds, &tv);
  if (n < 0) {
    SocketHandle sockfd = connect_sockfd_;
    connect_sockfd_ = kInvalidSocket;
    Retry(sockfd);
    return;
  }
  if (n == 0) {
    if (connect_) {
      loop_->RunAfter(0.01, false,
                       std::bind(&Connector::PollConnect, shared_from_this()));
    }
    return;
  }

  SocketHandle sockfd = connect_sockfd_;
  connect_sockfd_ = kInvalidSocket;
  FinishConnect(sockfd);
}

void Connector::FinishConnect(SocketHandle sockfd) {
  int err = socket::GetSocketError(sockfd);
  if (err) {
    Retry(sockfd);
    return;
  }
  if (socket::IsSelfConnect(sockfd)) {
    Retry(sockfd);
    return;
  }

  SetState(kConnected);
  if (connect_) {
    if (new_connection_callback_) {
      new_connection_callback_(sockfd);
    } else {
      socket::Close(sockfd);
    }
  } else {
    socket::Close(sockfd);
  }
}
#endif

void Connector::Retry(SocketHandle sockfd) {
#ifdef _WIN32
  connect_sockfd_ = kInvalidSocket;
  loop_->ReleaseSocketContext(sockfd);
#endif
  socket::Close(sockfd);
  SetState(kDisconnected);
  if (!retry_) {
    if (error_connection_callback_) {
      error_connection_callback_();
    }
    return;
  }

  if (connect_) {
    LOG_WARN << "Connector::Retry - Retry Connecting to " << ip_ << " " << port_
             << " in " << retry_delay_ms_ << " milliseconds. ";
    loop_->RunAfter(
        retry_delay_ms_ / 1000.0, false,
        std::bind(&Connector::StartInLoop, shared_from_this(), false));
#ifdef _WIN32
    retry_delay_ms_ = (retry_delay_ms_ * 2) < (kMaxRetryDelayMs)
                          ? (retry_delay_ms_ * 2)
                          : (kMaxRetryDelayMs);
#else
    retry_delay_ms_ = std::min(retry_delay_ms_ * 2, kMaxRetryDelayMs);
#endif
  } else {
    LOG_DEBUG << "do not connect_";
  }
}

void Connector::Restart() {
  loop_->AssertInLoopThread();
  SetState(kDisconnected);
  retry_delay_ms_ = kInitRetryDelayMs;
  connect_ = true;
  StartInLoop(false);
}

void Connector::HandleWrite() {
  if (state_ == kConnecting) {
    SocketHandle sockfd = RemoveAndResetChannel();
    int err = socket::GetSocketError(sockfd);
    if (err) {
      Retry(sockfd);
    } else if (socket::IsSelfConnect(sockfd)) {
      Retry(sockfd);
    } else {
      SetState(kConnected);
      if (connect_) {
        if (new_connection_callback_) {
          new_connection_callback_(sockfd);
        } else {
#ifdef _WIN32
          loop_->ReleaseSocketContext(sockfd);
#endif
          socket::Close(sockfd);
        }
      } else {
#ifdef _WIN32
        loop_->ReleaseSocketContext(sockfd);
#endif
        socket::Close(sockfd);
      }
    }
  } else {
    assert(state_ == kDisconnected);
  }
}

void Connector::HandleError() {
  if (state_ == kConnecting) {
    SocketHandle sockfd = RemoveAndResetChannel();
    int err = socket::GetSocketError(sockfd);
    LOG_TRACE << "SO_ERROR = " << err << " " << strerror(err);
    Retry(sockfd);
  }
}

void Connector::Connecting(bool state) {
  SocketHandle sockfd = socket::CreateNonblockingOrDie();
  if (!socket::IsValid(sockfd)) {
    Retry(sockfd);
    return;
  }

  int32_t ret = socket::Connect(sockfd, ip_.c_str(), port_);
  int32_t saved_errno = GetConnectError(ret);

  switch (saved_errno) {
    case 0:
    case EISCONN:
      SetState(kConnecting);
      Connecting(state, sockfd);
      break;
    case EINPROGRESS:
    case EINTR:
      if (state && !socket::ConnectWaitReady(sockfd, kHeart * 1000)) {
        Retry(sockfd);
        break;
      }
      SetState(kConnecting);
      Connecting(state, sockfd);
      break;
    case EAGAIN:
    case EADDRINUSE:
    case EADDRNOTAVAIL:
    case ECONNREFUSED:
    case ENETUNREACH:
      Retry(sockfd);
      break;
    case EACCES:
    case EPERM:
    case EAFNOSUPPORT:
    case EALREADY:
    case EBADF:
    case EFAULT:
    case ENOTSOCK:
      Retry(sockfd);
      break;
    default:
      Retry(sockfd);
      break;
  }
}

}  // namespace zrpc
