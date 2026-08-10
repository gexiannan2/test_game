#include <charconv>
#include <cstdlib>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

#include "zrpc/base/buffer.h"
#include "zrpc/base/logger.h"
#include "zrpc/net/event_loop.h"
#include "zrpc/net/tcp_connection.h"
#include "zrpc/net/tcp_server.h"

#ifdef _WIN32
#include <winsock2.h>
#endif

namespace {
constexpr uint16_t kDefaultPort = 19081;

uint16_t ParsePort(std::string_view value) {
  uint32_t port = 0;
  const auto parsed =
      std::from_chars(value.data(), value.data() + value.size(), port);
  if (parsed.ec != std::errc() ||
      parsed.ptr != value.data() + value.size() || port == 0 ||
      port > std::numeric_limits<uint16_t>::max()) {
    return kDefaultPort;
  }
  return static_cast<uint16_t>(port);
}

void OnMessage(const std::shared_ptr<zrpc::TcpConnection>& conn,
               zrpc::Buffer* buf) {
  if (buf->ReadableBytes() == 0) {
    return;
  }
  LOG_INFO << "[server] recv " << buf->ReadableBytes()
           << " bytes from fd=" << conn->GetSockfd() << ": "
           << buf->ToStringView();
  conn->Send(buf->Peek(), static_cast<int>(buf->ReadableBytes()));
  LOG_INFO << "[server] echo back to fd=" << conn->GetSockfd();
  buf->RetrieveAll();
}

void OnConnection(const std::shared_ptr<zrpc::TcpConnection>& conn) {
  if (conn->Connected()) {
    LOG_INFO << "[server] new connection fd=" << conn->GetSockfd();
    return;
  }
  LOG_INFO << "[server] connection closed fd=" << conn->GetSockfd();
}
}  // namespace

int main(int argc, char* argv[]) {
#ifdef _WIN32
  WSADATA wsa_data{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
    return 1;
  }
#endif

  uint16_t port = kDefaultPort;
  if (argc > 1) {
    port = ParsePort(argv[1]);
  }

  zrpc::EventLoop loop;
  zrpc::TcpServer server(&loop, "127.0.0.1", static_cast<int16_t>(port), nullptr);
  server.SetThreadNum(0);
  server.SetConnectionCallback(OnConnection);
  server.SetMessageCallback(OnMessage);
  server.Start();

  LOG_INFO << "[server] tcp echo server listening on 127.0.0.1:" << port;
  loop.Run();

#ifdef _WIN32
  WSACleanup();
#endif
  return 0;
}
