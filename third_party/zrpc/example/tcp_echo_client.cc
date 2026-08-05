#include <atomic>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

#include "zrpc/base/buffer.h"
#include "zrpc/base/logger.h"
#include "zrpc/net/event_loop.h"
#include "zrpc/net/tcp_client.h"
#include "zrpc/net/tcp_connection.h"

#ifdef _WIN32
#include <winsock2.h>
#endif

namespace {
constexpr uint16_t kDefaultPort = 19081;
const char kPing[] = "zrpc-tcp-ping";

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

  std::atomic<bool> success{false};
  zrpc::EventLoop loop;
  zrpc::TcpClient client(&loop, "127.0.0.1", static_cast<int16_t>(port), nullptr);

  auto shutdown = [&]() {
    client.Stop();
    loop.Quit();
  };

  client.SetConnectionCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn) {
    if (conn->Connected()) {
      LOG_INFO << "[client] connected to 127.0.0.1:" << port
               << " fd=" << conn->GetSockfd();
      LOG_INFO << "[client] send ping: " << kPing;
      conn->Send(kPing, static_cast<int>(sizeof(kPing) - 1));
      return;
    }
    LOG_INFO << "[client] disconnected fd=" << conn->GetSockfd();
    loop.QueueInLoop(shutdown);
  });

  client.SetMessageCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn,
                                zrpc::Buffer* buf) {
    const size_t msg_len = sizeof(kPing) - 1;
    if (buf->ReadableBytes() < static_cast<int32_t>(msg_len)) {
      return;
    }
    LOG_INFO << "[client] recv echo: " << buf->ToStringView();
    if (std::memcmp(buf->Peek(), kPing, msg_len) == 0) {
      LOG_INFO << "[client] echo verified OK, shutting down";
      success.store(true);
      conn->Shutdown();
      loop.QueueInLoop(shutdown);
    }
  });

  LOG_INFO << "[client] connecting to 127.0.0.1:" << port;

  client.Connect();
  loop.RunAfter(5.0, false, [&]() { loop.QueueInLoop(shutdown); });
  loop.Run();

#ifdef _WIN32
  WSACleanup();
#endif
  return success.load() ? 0 : 1;
}
