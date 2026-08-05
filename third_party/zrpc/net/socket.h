#pragma once

#ifdef _WIN32
#include <WinSock2.h>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SSIZE_T ssize_t;
#define IOV_TYPE WSABUF
#define STRCMP _stricmp
#define MEMCMP _strnicmp
#include <filesystem>
#else
#define IOV_TYPE struct iovec
#define STRCMP strcasecmp
#define MEMCMP strncasecmp

#include <arpa/inet.h>
#include <fcntl.h>
#include <dirent.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pwd.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <libkern/OSByteOrder.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/event.h>
#include <sys/poll.h>
#include <sys/un.h>
#include <string_view>
//#include <zlib.h>

#define htobe16(x) OSSwapHostToBigInt16(x)
#define htole16(x) OSSwapHostToLittleInt16(x)
#define be16toh(x) OSSwapBigToHostInt16(x)
#define le16toh(x) OSSwapLittleToHostInt16(x)

#define htobe32(x) OSSwapHostToBigInt32(x)
#define htole32(x) OSSwapHostToLittleInt32(x)
#define be32toh(x) OSSwapBigToHostInt32(x)
#define le32toh(x) OSSwapLittleToHostInt32(x)

#define htobe64(x) OSSwapHostToBigInt64(x)
#define htole64(x) OSSwapHostToLittleInt64(x)
#define be64toh(x) OSSwapBigToHostInt64(x)
#define le64toh(x) OSSwapLittleToHostInt64(x)
#endif

#ifdef __linux__
#include <endian.h>
#include <linux/tcp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/poll.h>
#include <sys/sendfile.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <sys/utsname.h>

#include <experimental/filesystem>
#endif
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string>
#include <string_view>

namespace zrpc {
#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

namespace socket {
constexpr int32_t kDefaultSocketBufSize = 256 * 1024;

inline bool IsValid(SocketHandle sockfd) {
  return sockfd != kInvalidSocket;
}

uint64_t HostToNetwork64(uint64_t host64);
uint32_t HostToNetwork32(uint32_t host32);
uint16_t HostToNetwork16(uint16_t host16);
uint64_t NetworkToHost64(uint64_t net64);
uint32_t NetworkToHost32(uint32_t net32);
uint16_t NetworkToHost16(uint16_t net16);

int32_t Pipe(SocketHandle fildes[2]);

SocketHandle Accept(SocketHandle &sockfd);

ssize_t Read(SocketHandle sockfd, void *buf, int32_t count);
ssize_t Readv(SocketHandle sockfd, IOV_TYPE *iov, int32_t iovcnt);
ssize_t Write(SocketHandle sockfd, const void *buf, int32_t count);

int32_t Shutdown(SocketHandle sockfd);

void Close(SocketHandle sockfd);
struct sockaddr_in6 GetPeerAddr(SocketHandle sockfd);
struct sockaddr_in6 GetLocalAddr(SocketHandle sockfd);

void ToIpPort(char *buf, size_t size, const struct sockaddr *addr);
void ToIp(char *buf, size_t size, const struct sockaddr *addr);
void ToPort(uint16_t *port, const struct sockaddr *addr);

void FromIpPort(const char *ip, uint16_t port, struct sockaddr_in *addr);
void FromIpPort(const char *ip, uint16_t port, struct sockaddr_in6 *addr);

SocketHandle CreateNonblockingOrDie();
SocketHandle CreateSocket();
SocketHandle CreateTcpSocket(const char *ip, int16_t port);
int32_t GetSocketError(SocketHandle sockfd);

int32_t Connect(SocketHandle sockfd, struct sockaddr *sin);
int32_t Connect(SocketHandle sockfd, const char *ip, int16_t port);
bool ConnectWaitReady(SocketHandle fd, int32_t msec);

bool IsSelfConnect(SocketHandle sockfd);
bool SetKeepAlive(SocketHandle fd, int32_t interval);
bool SetSocketNonBlock(SocketHandle sockfd);
bool SetSocketBlock(SocketHandle sockfd);
bool SetTcpNoDelay(SocketHandle sockfd, bool on);
bool SetSocketBufferSize(SocketHandle sockfd, int32_t bytes);
bool SetTimeOut(SocketHandle sockfd, const struct timeval tc);
void SetReuseAddr(SocketHandle sockfd, bool on);
void SetReusePort(SocketHandle sockfd, bool on);
bool Resolve(std::string_view hostname, struct sockaddr_in *out);
bool Resolve(std::string_view hostname, struct sockaddr_in6 *out);
};  // namespace socket
}  // namespace zrpc
