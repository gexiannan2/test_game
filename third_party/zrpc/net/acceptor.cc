#include "zrpc/net/acceptor.h"

#include <exception>

#include "zrpc/base/log.h"
#include "zrpc/base/logger.h"

namespace zrpc {
Acceptor::Acceptor(EventLoop *loop, const std::string &ip, int16_t port)
    : loop_(loop),
      channel_(loop_, socket::CreateTcpSocket(ip.c_str(), port)),
      sockfd_(channel_.Getfd()),
#ifndef _WIN32
      idlefd_(::open("/dev/null", O_RDONLY | O_CLOEXEC)),
#endif
      listenning_(false) {
#ifndef _WIN32
  assert(idlefd_ >= 0);
#endif
  channel_.SetReadCallback(std::bind(&Acceptor::HandleRead, this));
}

Acceptor::~Acceptor() {
#ifndef _WIN32
  if (idlefd_ >= 0) {
    ::close(idlefd_);
    idlefd_ = -1;
  }
#endif
  if (!socket::IsValid(sockfd_)) {
    return;
  }
#ifdef _WIN32
  const bool registered = channel_.IsAddedToLoop();
#endif
  if (listenning_) {
    channel_.DisableAll();
    channel_.Remove();
  }
#ifdef _WIN32
  if (registered) {
    loop_->ReleaseSocketContext(sockfd_);
  }
#endif
  socket::Close(sockfd_);
  sockfd_ = kInvalidSocket;
}

void Acceptor::HandleRead() {
  loop_->AssertInLoopThread();
  SocketHandle connfd = socket::Accept(sockfd_);
  if (socket::IsValid(connfd)) {
    if (new_connection_callback_) {
      if (!socket::SetSocketNonBlock(connfd)) {
        socket::Close(connfd);
        return;
      }
      try {
        new_connection_callback_(connfd);
      } catch (const std::exception& ex) {
        LOG_WARN << "Acceptor new-connection callback threw: " << ex.what();
        socket::Close(connfd);
      } catch (...) {
        LOG_WARN << "Acceptor new-connection callback threw";
        socket::Close(connfd);
      }
    } else {
      socket::Close(connfd);
    }
#ifndef _WIN32
  } else if (errno == EMFILE && idlefd_ >= 0) {
    ::close(idlefd_);
    connfd = socket::Accept(sockfd_);
    if (socket::IsValid(connfd)) {
      socket::Close(connfd);
    }
    idlefd_ = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
#endif
  }
}

bool Acceptor::Listen() {
  loop_->AssertInLoopThread();
  if (!socket::IsValid(sockfd_)) {
    return false;
  }
  listenning_ = true;
  channel_.EnableReading();
  if (!loop_->HasChannel(&channel_)) {
    listenning_ = false;
    return false;
  }
  return true;
}

void Acceptor::StopListening() {
  loop_->AssertInLoopThread();
  if (!listenning_) {
    return;
  }
  listenning_ = false;
  channel_.DisableAll();
  channel_.Remove();
#ifdef _WIN32
  loop_->ReleaseSocketContext(sockfd_);
#endif
  socket::Close(sockfd_);
  sockfd_ = kInvalidSocket;
}
}  // namespace zrpc
