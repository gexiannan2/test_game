// Minimal raw TCP echo throughput test (no zrpc). Compare ceiling vs framework.
#include <chrono>
#include <charconv>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using ssize_t = SSIZE_T;
using sock_t = SOCKET;
#define CLOSE_SOCKET closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
#define INVALID_SOCKET (-1)
#define CLOSE_SOCKET close
#endif

namespace {
constexpr int kPort = 19700;
constexpr int kSeconds = 10;
constexpr int kMaxMessageSize = 16 * 1024 * 1024;
constexpr int kMaxPipeline = 65536;
constexpr int kMaxPipelinedBytes = 64 * 1024;

bool IsValidSocket(sock_t fd) { return fd != INVALID_SOCKET; }

int LastSocketError() {
#ifdef _WIN32
  return WSAGetLastError();
#else
  return errno;
#endif
}

bool SetNoDelay(sock_t fd) {
  int one = 1;
  return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                    reinterpret_cast<const char*>(&one), sizeof(one)) == 0;
}

#ifdef _WIN32
bool InitNet() {
  WSADATA wsa{};
  return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}
void CleanupNet() { WSACleanup(); }
#else
bool InitNet() { return true; }
void CleanupNet() {}
#endif

ssize_t SendAll(sock_t fd, const char* data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
#ifdef _WIN32
    int n = ::send(fd, data + sent, static_cast<int>(len - sent), 0);
#else
#ifdef MSG_NOSIGNAL
    ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
#else
    ssize_t n = ::send(fd, data + sent, len - sent, 0);
#endif
#endif
    if (n < 0) {
#ifdef _WIN32
      if (WSAGetLastError() == WSAEINTR) continue;
#else
      if (errno == EINTR) continue;
#endif
      return n;
    }
    if (n == 0) return 0;
    sent += static_cast<size_t>(n);
  }
  return static_cast<ssize_t>(sent);
}

ssize_t RecvAll(sock_t fd, char* data, size_t len) {
  size_t got = 0;
  while (got < len) {
#ifdef _WIN32
    int n = ::recv(fd, data + got, static_cast<int>(len - got), 0);
#else
    ssize_t n = ::recv(fd, data + got, len - got, 0);
#endif
    if (n < 0) {
#ifdef _WIN32
      if (WSAGetLastError() == WSAEINTR) continue;
#else
      if (errno == EINTR) continue;
#endif
      return n;
    }
    if (n == 0) return 0;
    got += static_cast<size_t>(n);
  }
  return static_cast<ssize_t>(got);
}

bool WaitForConnection(sock_t listen_fd) {
  fd_set read_set;
  FD_ZERO(&read_set);
  FD_SET(listen_fd, &read_set);
  timeval timeout{};
  timeout.tv_sec = 5;
#ifdef _WIN32
  return select(0, &read_set, nullptr, nullptr, &timeout) > 0;
#else
  return select(listen_fd + 1, &read_set, nullptr, nullptr, &timeout) > 0;
#endif
}

bool RunServer(int msg_size) {
  sock_t listen_fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (!IsValidSocket(listen_fd)) {
    fprintf(stderr, "socket failed: %d\n", LastSocketError());
    return false;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(kPort);
  int reuse = 1;
  if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&reuse), sizeof(reuse)) != 0 ||
      bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
      listen(listen_fd, 1) != 0) {
    fprintf(stderr, "server setup failed: %d\n", LastSocketError());
    CLOSE_SOCKET(listen_fd);
    return false;
  }
  if (!WaitForConnection(listen_fd)) {
    fprintf(stderr, "server accept timed out\n");
    CLOSE_SOCKET(listen_fd);
    return false;
  }

  sock_t conn = accept(listen_fd, nullptr, nullptr);
  if (!IsValidSocket(conn)) {
    fprintf(stderr, "accept failed: %d\n", LastSocketError());
    CLOSE_SOCKET(listen_fd);
    return false;
  }
  if (!SetNoDelay(conn)) {
    fprintf(stderr, "TCP_NODELAY failed: %d\n", LastSocketError());
  }
  CLOSE_SOCKET(listen_fd);

  std::vector<char> buf(static_cast<size_t>(msg_size));
  while (true) {
    if (RecvAll(conn, buf.data(), static_cast<size_t>(msg_size)) <= 0) break;
    if (SendAll(conn, buf.data(), static_cast<size_t>(msg_size)) <= 0) break;
  }
  CLOSE_SOCKET(conn);
  return true;
}

bool RunClient(int msg_size, int pipeline) {
  sock_t fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (!IsValidSocket(fd)) {
    fprintf(stderr, "socket failed: %d\n", LastSocketError());
    return false;
  }
  if (!SetNoDelay(fd)) {
    fprintf(stderr, "TCP_NODELAY failed: %d\n", LastSocketError());
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(kPort);
  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    fprintf(stderr, "connect failed: %d\n", LastSocketError());
    CLOSE_SOCKET(fd);
    return false;
  }

  std::vector<char> payload(static_cast<size_t>(msg_size), 'x');
  std::vector<char> recv_buf(static_cast<size_t>(msg_size));

  auto send_one = [&]() {
    return SendAll(fd, payload.data(), static_cast<size_t>(msg_size)) ==
           msg_size;
  };

  for (int i = 0; i < pipeline; ++i) {
    if (!send_one()) {
      CLOSE_SOCKET(fd);
      return false;
    }
  }

  using Clock = std::chrono::steady_clock;
  const auto t0 = Clock::now();
  uint64_t round_trips = 0;
  const auto deadline = t0 + std::chrono::seconds(kSeconds);

  while (Clock::now() < deadline) {
    if (RecvAll(fd, recv_buf.data(), static_cast<size_t>(msg_size)) <= 0) break;
    ++round_trips;
    if (!send_one()) break;
  }
  const auto t1 = Clock::now();

  const double sec =
      std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0).count();
  const double mib = (static_cast<double>(round_trips) * msg_size) / sec / 1024.0 / 1024.0;
  const double gib = mib / 1024.0;

  printf("[raw] msg_size=%d pipeline=%d duration=%.3fs\n", msg_size, pipeline, sec);
  printf("[raw] round_trips=%llu msg/s=%.0f one_way=%.2f MiB/s (%.2f GiB/s) wire_echo~%.2f GiB/s\n",
         static_cast<unsigned long long>(round_trips), round_trips / sec, mib, gib,
         gib * 2.0);

  CLOSE_SOCKET(fd);
  return round_trips > 0;
}

bool ParsePositiveInt(const char* text, int upper_bound, int* value) {
  if (text == nullptr || value == nullptr) {
    return false;
  }
  const char* end = text + std::strlen(text);
  int parsed = 0;
  const auto result = std::from_chars(text, end, parsed);
  if (result.ec != std::errc() || result.ptr != end || parsed <= 0 ||
      parsed > upper_bound) {
    return false;
  }
  *value = parsed;
  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 3) {
    fprintf(stderr, "Usage: %s server|client <msg_size> [pipeline]\n", argv[0]);
    return 1;
  }
  int msg_size = 0;
  int pipeline = 8;
  if (!ParsePositiveInt(argv[2], kMaxMessageSize, &msg_size) ||
      (argc > 3 &&
       !ParsePositiveInt(argv[3], kMaxPipeline, &pipeline)) ||
      (pipeline > 1 &&
       msg_size > kMaxPipelinedBytes / pipeline)) {
    fprintf(stderr, "msg_size or pipeline is out of range\n");
    return 1;
  }
  const std::string mode(argv[1]);
  if (mode != "server" && mode != "client") {
    fprintf(stderr, "mode must be server or client\n");
    return 1;
  }
  if (!InitNet()) {
    fprintf(stderr, "network initialization failed\n");
    return 1;
  }
  bool success = false;
  if (mode == "server") {
    success = RunServer(msg_size);
  } else {
    bool server_success = false;
    std::thread t([&]() { server_success = RunServer(msg_size); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const bool client_success = RunClient(msg_size, pipeline);
    t.join();
    success = client_success && server_success;
  }
  CleanupNet();
  return success ? 0 : 1;
}
