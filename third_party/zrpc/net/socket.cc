#include "zrpc/net/socket.h"

#include <cassert>

#include "zrpc/base/logger.h"

#ifdef _WIN32
#define ZRPC_SETSOCKOPT(s, lvl, optname, optval_ptr, optlen) \
  ::setsockopt(s, lvl, optname, reinterpret_cast<const char *>(optval_ptr), \
               static_cast<int>(optlen))
#else
#define ZRPC_SETSOCKOPT(s, lvl, optname, optval_ptr, optlen) \
  ::setsockopt(s, lvl, optname, optval_ptr, static_cast<socklen_t>(optlen))
#endif

namespace zrpc {
namespace {
void ApplyDefaultSocketOptions(SocketHandle sockfd) {
  if (!socket::IsValid(sockfd)) {
    return;
  }
  socket::SetSocketBufferSize(sockfd, socket::kDefaultSocketBufSize);
}
}  // namespace

bool socket::SetSocketBufferSize(SocketHandle sockfd, int32_t bytes) {
  if (!IsValid(sockfd) || bytes <= 0) {
    return false;
  }
  if (ZRPC_SETSOCKOPT(sockfd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes)) < 0) {
    return false;
  }
  if (ZRPC_SETSOCKOPT(sockfd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes)) < 0) {
    return false;
  }
  return true;
}

bool socket::Resolve(std::string_view hostname, struct sockaddr_in6 *out) {
  assert(out != nullptr);
  struct addrinfo hints {};
  hints.ai_family = AF_INET6;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo *result = nullptr;
  const std::string host(hostname);
  const int ret = ::getaddrinfo(host.c_str(), nullptr, &hints, &result);
  if (ret != 0 || result == nullptr) {
    if (result != nullptr) {
      ::freeaddrinfo(result);
    }
    return false;
  }
  const auto *addr = reinterpret_cast<struct sockaddr_in6 *>(result->ai_addr);
  out->sin6_family = AF_INET6;
  out->sin6_addr = addr->sin6_addr;
  ::freeaddrinfo(result);
  return true;
}

bool socket::Resolve(std::string_view hostname, struct sockaddr_in *out) {
  assert(out != nullptr);
  struct addrinfo hints {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo *result = nullptr;
  const std::string host(hostname);
  const int ret = ::getaddrinfo(host.c_str(), nullptr, &hints, &result);
  if (ret != 0 || result == nullptr) {
    if (result != nullptr) {
      ::freeaddrinfo(result);
    }
    return false;
  }
  const auto *addr = reinterpret_cast<struct sockaddr_in *>(result->ai_addr);
  out->sin_family = AF_INET;
  out->sin_addr = addr->sin_addr;
  ::freeaddrinfo(result);
  return true;
}

ssize_t socket::Readv(SocketHandle sockfd, IOV_TYPE *iov, int32_t iovcnt) {
  if (!IsValid(sockfd) || iov == nullptr || iovcnt <= 0) {
#ifdef _WIN32
    ::WSASetLastError(WSAEINVAL);
#else
    errno = EINVAL;
#endif
    return -1;
  }
#ifdef _WIN32
  DWORD bytesRead;
  DWORD flags = 0;
  if (::WSARecv(sockfd, iov, iovcnt, &bytesRead, &flags, nullptr, nullptr)) {
    if (WSAGetLastError() == WSAECONNABORTED)
      return 0;
    else
      return -1;
  } else {
    return bytesRead;
  }
#else
  return ::readv(sockfd, iov, iovcnt);
#endif
}

ssize_t socket::Read(SocketHandle sockfd, void *buf, int32_t count) {
  if (!IsValid(sockfd) || buf == nullptr || count < 0) {
#ifdef _WIN32
    ::WSASetLastError(WSAEINVAL);
#else
    errno = EINVAL;
#endif
    return -1;
  }
#ifdef __linux__
  return ::read(sockfd, static_cast<char *>(buf), count);
#endif

#ifdef __APPLE__
  return ::read(sockfd, static_cast<char *>(buf), count);
#endif

#ifdef _WIN32
  return ::recv(sockfd, static_cast<char *>(buf), count, 0);
#endif
}

ssize_t socket::Write(SocketHandle sockfd, const void *buf, int32_t count) {
  if (!IsValid(sockfd) || (buf == nullptr && count != 0) || count < 0) {
#ifdef _WIN32
    ::WSASetLastError(WSAEINVAL);
#else
    errno = EINVAL;
#endif
    return -1;
  }
#ifdef __linux__
  const ssize_t n =
      ::send(sockfd, buf, static_cast<size_t>(count), MSG_NOSIGNAL);
  if (n < 0 && errno == ENOTSOCK) {
    // eventfd 等非套接字描述符仍需使用 write。
    return ::write(sockfd, buf, static_cast<size_t>(count));
  }
  return n;
#endif

#ifdef __APPLE__
  return ::send(sockfd, buf, static_cast<size_t>(count), 0);
#endif

#ifdef _WIN32
  return ::send(sockfd, static_cast<const char *>(buf), count, 0);
#endif
}

SocketHandle socket::Accept(SocketHandle &sockfd) {
  struct sockaddr_in6 address;
  socklen_t len = sizeof(address);
#ifdef __linux__
  SocketHandle connfd = ::accept4(sockfd, (struct sockaddr *)&address, &len,
                                  SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
  SocketHandle connfd = ::accept(sockfd, (struct sockaddr *)&address, &len);
#endif
  if (IsValid(connfd)) {
#ifdef __APPLE__
    const int flags = ::fcntl(connfd, F_GETFD, 0);
    if (flags < 0 || ::fcntl(connfd, F_SETFD, flags | FD_CLOEXEC) < 0) {
      Close(connfd);
      return kInvalidSocket;
    }
#endif
    ApplyDefaultSocketOptions(connfd);
  }
  return connfd;
}

int32_t socket::Pipe(SocketHandle fildes[2]) {
  if (fildes == nullptr) {
    return -1;
  }
  fildes[0] = kInvalidSocket;
  fildes[1] = kInvalidSocket;
  SocketHandle tcp1 = kInvalidSocket;
  SocketHandle tcp2 = kInvalidSocket;
  sockaddr_in name;
  memset(&name, 0, sizeof(name));
  name.sin_family = AF_INET;
  name.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t namelen = sizeof(name);
  SocketHandle tcp = CreateSocket();
  if (!IsValid(tcp)) {
    return -1;
  }
  if (::bind(tcp, (sockaddr *)&name, namelen) != 0 ||
      ::listen(tcp, 5) != 0 ||
      ::getsockname(tcp, (sockaddr *)&name, &namelen) != 0) {
    Close(tcp);
    return -1;
  }

  tcp1 = CreateSocket();
  if (!IsValid(tcp1) || Connect(tcp1, (sockaddr *)&name) != 0) {
    if (IsValid(tcp1)) {
      Close(tcp1);
    }
    Close(tcp);
    return -1;
  }

  tcp2 = ::accept(tcp, (sockaddr *)&name, &namelen);
  if (!IsValid(tcp2)) {
    Close(tcp1);
    Close(tcp);
    return -1;
  }
#ifndef _WIN32
  const int tcp2_flags = ::fcntl(tcp2, F_GETFD, 0);
  if (tcp2_flags < 0 ||
      ::fcntl(tcp2, F_SETFD, tcp2_flags | FD_CLOEXEC) < 0) {
    Close(tcp1);
    Close(tcp2);
    Close(tcp);
    return -1;
  }
#endif
  Close(tcp);

  if (!SetSocketNonBlock(tcp1) || !SetSocketNonBlock(tcp2)) {
    Close(tcp1);
    Close(tcp2);
    return -1;
  }
  fildes[0] = tcp1;
  fildes[1] = tcp2;
  return 1;
}

uint64_t socket::HostToNetwork64(uint64_t host64) {
#ifdef _WIN32
  uint64_t ret = 0;
  uint32_t high, low;

  low = host64 & 0xFFFFFFFF;
  high = (host64 >> 32) & 0xFFFFFFFF;
  low = htonl(low);
  high = htonl(high);
  ret = low;
  ret <<= 32;
  ret |= high;
  return ret;
#else
  return htobe64(host64);
#endif
}

uint32_t socket::HostToNetwork32(uint32_t host32) {
#ifdef _WIN32
  return htonl(host32);
#else
  return htobe32(host32);
#endif
}

uint16_t socket::HostToNetwork16(uint16_t host16) {
#ifdef _WIN32
  return htons(host16);
#else
  return htobe16(host16);
#endif
}

int32_t socket::Shutdown(SocketHandle sockfd) {
  if (!IsValid(sockfd)) {
    return -1;
  }
#ifdef _WIN32
  return ::shutdown(sockfd, SD_SEND);
#else
  return ::shutdown(sockfd, SHUT_WR);
#endif
}

uint64_t socket::NetworkToHost64(uint64_t net64) {
#ifdef _WIN32
  uint64_t ret = 0;
  uint32_t high, low;

  low = net64 & 0xFFFFFFFF;
  high = (net64 >> 32) & 0xFFFFFFFF;
  low = ntohl(low);
  high = ntohl(high);

  ret = low;
  ret <<= 32;
  ret |= high;
  return ret;
#else
  return be64toh(net64);
#endif
}

uint32_t socket::NetworkToHost32(uint32_t net32) {
#ifdef _WIN32
  return ntohl(net32);
#else
  return be32toh(net32);
#endif
}

uint16_t socket::NetworkToHost16(uint16_t net16) {
#ifdef _WIN32
  return ntohs(net16);
#else
  return be16toh(net16);
#endif
}

void socket::Close(SocketHandle sockfd) {
  if (!IsValid(sockfd)) {
    return;
  }
#ifdef _WIN32
  ::closesocket(sockfd);
#else
  ::close(sockfd);
#endif
}
struct sockaddr_in6 socket::GetLocalAddr(SocketHandle sockfd) {
  struct sockaddr_in6 localaddr;
  memset(&localaddr, 0, sizeof localaddr);
  socklen_t addrlen = static_cast<socklen_t>(sizeof localaddr);
  if (::getsockname(sockfd, (struct sockaddr *)&localaddr, &addrlen) < 0) {
    LOG_WARN << "";
  }
  return localaddr;
}

struct sockaddr_in6 socket::GetPeerAddr(SocketHandle sockfd) {
  struct sockaddr_in6 peeraddr;
  memset(&peeraddr, 0, sizeof peeraddr);
  socklen_t addrlen = static_cast<socklen_t>(sizeof peeraddr);
  if (::getpeername(sockfd, (struct sockaddr *)&peeraddr, &addrlen) < 0) {
    LOG_WARN << "";
  }
  return peeraddr;
}

bool socket::IsSelfConnect(SocketHandle sockfd) {
  struct sockaddr_in6 localaddr = GetLocalAddr(sockfd);
  struct sockaddr_in6 peeraddr = GetPeerAddr(sockfd);
  if (localaddr.sin6_family == AF_INET) {
    const struct sockaddr_in *laddr4 =
        reinterpret_cast<struct sockaddr_in *>(&localaddr);
    const struct sockaddr_in *raddr4 =
        reinterpret_cast<struct sockaddr_in *>(&peeraddr);
    return laddr4->sin_port == raddr4->sin_port &&
           laddr4->sin_addr.s_addr == raddr4->sin_addr.s_addr;
  } else if (localaddr.sin6_family == AF_INET6) {
    return localaddr.sin6_port == peeraddr.sin6_port &&
           memcmp(&localaddr.sin6_addr, &peeraddr.sin6_addr,
                  sizeof localaddr.sin6_addr) == 0;
  } else {
    return false;
  }
}

int32_t socket::GetSocketError(SocketHandle sockfd) {
  int32_t optval;
  socklen_t optlen = static_cast<socklen_t>(sizeof optval);

  if (::getsockopt(sockfd, SOL_SOCKET, SO_ERROR, (char *)&optval, &optlen) <
      0) {
#ifdef _WIN32
    return static_cast<int32_t>(::WSAGetLastError());
#else
    return errno;
#endif
  } else {
    return optval;
  }
}

void socket::ToIpPort(char *buf, size_t size, const struct sockaddr *addr) {
  ToIp(buf, size, addr);
  size_t end = ::strlen(buf);
  const struct sockaddr_in *addr4 = (const struct sockaddr_in *)(addr);
  uint16_t port = NetworkToHost16(addr4->sin_port);
  assert(size > end);
  snprintf(buf + end, size - end, ":%u", port);
}

void socket::ToPort(uint16_t *port, const struct sockaddr *addr) {
  const struct sockaddr_in *addr4 = (const struct sockaddr_in *)(addr);
  *port = NetworkToHost16(addr4->sin_port);
}

void socket::ToIp(char *buf, size_t size, const struct sockaddr *addr) {
  if (addr->sa_family == AF_INET) {
    assert(size >= INET_ADDRSTRLEN);
    const struct sockaddr_in *addr4 = (const struct sockaddr_in *)(addr);
    ::inet_ntop(AF_INET, &addr4->sin_addr, buf, static_cast<socklen_t>(size));
  } else if (addr->sa_family == AF_INET6) {
    assert(size >= INET6_ADDRSTRLEN);
    const struct sockaddr_in6 *addr6 = (const struct sockaddr_in6 *)(addr);
    ::inet_ntop(AF_INET6, &addr6->sin6_addr, buf, static_cast<socklen_t>(size));
  }
}

void socket::FromIpPort(const char *ip, uint16_t port,
                        struct sockaddr_in *addr) {
  addr->sin_family = AF_INET;
  addr->sin_port = HostToNetwork16(port);
  if (::inet_pton(AF_INET, ip, &addr->sin_addr) <= 0) {
    LOG_WARN << "socket::FromIpPort";
  }
}

void socket::FromIpPort(const char *ip, uint16_t port,
                        struct sockaddr_in6 *addr) {
  addr->sin6_family = AF_INET6;
  addr->sin6_port = HostToNetwork16(port);
  if (::inet_pton(AF_INET6, ip, &addr->sin6_addr) <= 0) {
    LOG_WARN << "socket::FromIpPort";
  }
}

SocketHandle socket::CreateSocket() {
  SocketHandle sockfd = kInvalidSocket;
#ifdef _WIN32
  sockfd = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0,
                        WSA_FLAG_OVERLAPPED);
#endif

#ifdef __linux__
  sockfd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
#endif

#ifdef __APPLE__
  sockfd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (IsValid(sockfd)) {
    const int fd_flags = ::fcntl(sockfd, F_GETFD, 0);
    const int no_sigpipe = 1;
    if (fd_flags < 0 ||
        ::fcntl(sockfd, F_SETFD, fd_flags | FD_CLOEXEC) < 0 ||
        ::setsockopt(sockfd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe,
                     sizeof(no_sigpipe)) < 0) {
      Close(sockfd);
      sockfd = kInvalidSocket;
    }
  }
#endif
  if (IsValid(sockfd)) {
    ApplyDefaultSocketOptions(sockfd);
  }
  return sockfd;
}

SocketHandle socket::CreateNonblockingOrDie() {
  SocketHandle sockfd = CreateSocket();
  if (!IsValid(sockfd)) {
    return sockfd;
  }
  if (!SetSocketNonBlock(sockfd)) {
    Close(sockfd);
    return kInvalidSocket;
  }
  return sockfd;
}

bool socket::ConnectWaitReady(SocketHandle fd, int32_t msec) {
  if (!IsValid(fd)) {
    return false;
  }
#ifdef _WIN32
  fd_set wfds;
  fd_set efds;
  FD_ZERO(&wfds);
  FD_ZERO(&efds);
  FD_SET(static_cast<SOCKET>(fd), &wfds);
  FD_SET(static_cast<SOCKET>(fd), &efds);
  timeval timeout;
  timeout.tv_sec = msec / 1000;
  timeout.tv_usec = (msec % 1000) * 1000;
  int32_t res = ::select(0, nullptr, &wfds, &efds,
                         msec < 0 ? nullptr : &timeout);
  if (res == SOCKET_ERROR) {
    return false;
  }
  if (res == 0) {
    ::WSASetLastError(WSAETIMEDOUT);
    return false;
  }
#else
  struct pollfd wfd[1];
  wfd[0].fd = fd;
  wfd[0].events = POLLOUT;
  wfd[0].revents = 0;
  int32_t res;
  do {
    res = ::poll(wfd, 1, msec);
  } while (res < 0 && errno == EINTR);
  if (res < 0) {
    return false;
  }
  if (res == 0) {
    errno = ETIMEDOUT;
    return false;
  }
#endif
  const int32_t socket_error = GetSocketError(fd);
  if (socket_error == 0) {
    return true;
  }
#ifdef _WIN32
  ::WSASetLastError(socket_error);
#else
  errno = socket_error;
#endif
  return false;
}

int32_t socket::Connect(SocketHandle sockfd, const char *ip, int16_t port) {
  if (!IsValid(sockfd) || ip == nullptr) {
#ifdef _WIN32
    ::WSASetLastError(WSAEINVAL);
#else
    errno = EINVAL;
#endif
    return -1;
  }
  struct sockaddr_in sin;
  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_port = htons(port);
  if (::inet_pton(AF_INET, ip, &sin.sin_addr) != 1) {
#ifdef _WIN32
    ::WSASetLastError(WSAEINVAL);
#else
    errno = EINVAL;
#endif
    return -1;
  }
  return Connect(sockfd, (struct sockaddr *)&sin);
}

int32_t socket::Connect(SocketHandle sockfd, struct sockaddr *sin) {
  if (!IsValid(sockfd) || sin == nullptr) {
    return -1;
  }
  return ::connect(sockfd, sin, sizeof(*sin));
}

bool socket::SetTimeOut(SocketHandle sockfd, const struct timeval tv) {
#ifdef _WIN32
  if (tv.tv_sec < 0 || tv.tv_usec < 0) {
    return false;
  }
  const uint64_t timeout64 = static_cast<uint64_t>(tv.tv_sec) * 1000 +
                             static_cast<uint64_t>(tv.tv_usec) / 1000;
  const DWORD timeout =
      timeout64 > MAXDWORD ? MAXDWORD : static_cast<DWORD>(timeout64);
  if (::setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char *>(&timeout),
                   static_cast<int>(sizeof(timeout))) == -1) {
    return false;
  }
  if (::setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char *>(&timeout),
                   static_cast<int>(sizeof(timeout))) == -1) {
    return false;
  }
#else
  if (::setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char *>(&tv),
                   static_cast<int>(sizeof(tv))) == -1) {
    return false;
  }

  if (::setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char *>(&tv),
                   static_cast<int>(sizeof(tv))) == -1) {
    return false;
  }
#endif
  return true;
}

bool socket::SetKeepAlive(SocketHandle fd, int32_t interval) {
  if (!IsValid(fd) || interval <= 0) {
    return false;
  }
  int val = 1;

  if (ZRPC_SETSOCKOPT(fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val)) == -1) {
    return false;
  }
#ifdef __linux__
  /* Default settings are more or less garbage, with the keepalive time
   * set to 7200 by default on Linux. Modify settings to make the feature
   * actually useful. */

  /* Send first probe after interval. */
  val = interval;
  if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &val, sizeof(val)) < 0) {
    return false;
  }

  /* Send next probes after the specified interval. Note that we set the
   * delay as interval / 3, as we send three probes before detecting
   * an error (see the next setsockopt call). */
  val = interval / 3;
  if (val == 0) val = 1;
  if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &val, sizeof(val)) < 0) {
    return false;
  }

  /* Consider the socket in error state after three we send three ACK
   * probes without getting a reply. */
  val = 1;
  if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &val, sizeof(val)) < 0) {
    return false;
  }
#endif
  return true;
}

void socket::SetReuseAddr(SocketHandle sockfd, bool on) {
  int32_t optval = on ? 1 : 0;
  ZRPC_SETSOCKOPT(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval,
                  sizeof(optval));
  // FIXME CHECK
}

void socket::SetReusePort(SocketHandle sockfd, bool on) {
#ifdef SO_REUSEPORT
  int32_t optval = on ? 1 : 0;
  int32_t ret = ZRPC_SETSOCKOPT(sockfd, SOL_SOCKET, SO_REUSEPORT, &optval,
                                sizeof(optval));
  if (ret < 0 && on) {
    LOG_WARN << "SO_REUSEPORT failed.";
  }
#else
  (void)sockfd;
  (void)on;
#endif
}

SocketHandle socket::CreateTcpSocket(const char *ip, int16_t port) {
  if (ip == nullptr) {
    return kInvalidSocket;
  }
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  if (::inet_pton(AF_INET, ip, &sa.sin_addr) != 1) {
    return kInvalidSocket;
  }

  SocketHandle sockfd = CreateSocket();
  if (!IsValid(sockfd)) {
    LOG_WARN << "Create TCP socket failed";
    return kInvalidSocket;
  }

  if (!SetSocketNonBlock(sockfd)) {
    LOG_WARN << "Set listen socket non-blocking failed";
    Close(sockfd);
    return kInvalidSocket;
  }

  int32_t optval = 1;
#ifdef _WIN32
  SetReuseAddr(sockfd, true);
#else
  if (::setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) <
      0) {
    LOG_WARN << "Set SO_REUSEPORT socket failed! error " << strerror(errno);
    Close(sockfd);
    return kInvalidSocket;
  }
  if (::setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval)) <
      0) {
    LOG_WARN << "Set SO_REUSEPORT socket failed! error " << strerror(errno);
    Close(sockfd);
    return kInvalidSocket;
  }
#endif
  if (::bind(sockfd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
    LOG_WARN << "Bind bind socket failed! error " << strerror(errno);
    Close(sockfd);
    return kInvalidSocket;
  }

  if (::listen(sockfd, SOMAXCONN)) {
    LOG_WARN << "Listen Listen socket failed! error " << strerror(errno);
    Close(sockfd);
    return kInvalidSocket;
  }

#ifdef __linux__
  ::setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &optval,
               static_cast<socklen_t>(sizeof optval));
#endif
  ApplyDefaultSocketOptions(sockfd);
  return sockfd;
}

bool socket::SetTcpNoDelay(SocketHandle sockfd, bool on) {
  int32_t optval = on ? 1 : 0;
  if (ZRPC_SETSOCKOPT(sockfd, IPPROTO_TCP, TCP_NODELAY, &optval,
                      sizeof(optval)) < 0) {
    return false;
  }
  return true;
}

bool socket::SetSocketBlock(SocketHandle sockfd) {
#ifdef _WIN32
  u_long nonblock = 0;
  int32_t ret = ::ioctlsocket(sockfd, FIONBIO, &nonblock);
  if (ret < 0) {
    return false;
  }
  return true;
#else
  int32_t opt = ::fcntl(sockfd, F_GETFL);
  if (opt < 0) {
    return false;
  }

  opt = opt & ~O_NONBLOCK;
  if (::fcntl(sockfd, F_SETFL, opt) < 0) {
    return false;
  }
  return true;
#endif
}

bool socket::SetSocketNonBlock(SocketHandle sockfd) {
#ifdef _WIN32
  u_long nonblock = 1;
  int32_t ret = ::ioctlsocket(sockfd, FIONBIO, &nonblock);
  if (ret < 0) {
    return false;
  }
  return true;
#else
  int32_t opt = ::fcntl(sockfd, F_GETFL);
  if (opt < 0) {
    LOG_WARN << "fcntl F_GETFL) failed! error" << strerror(errno);
    return false;
  }

  opt = opt | O_NONBLOCK;
  if (::fcntl(sockfd, F_SETFL, opt) < 0) {
    LOG_WARN << "fcntl F_GETFL) failed! error" << strerror(errno);
    return false;
  }
  return true;
#endif
}

}  // namespace zrpc


