#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "zrpc/base/buffer.h"
#include "zrpc/base/logger.h"
#include "zrpc/net/event_loop.h"
#include "zrpc/net/tcp_client.h"
#include "zrpc/net/tcp_connection.h"
#include "zrpc/net/tcp_server.h"

#ifdef _WIN32
#include <winsock2.h>
#endif

namespace {
constexpr uint16_t kPort = 19181;
const char kPing[] = "zrpc-tcp-ping";

void OnServerMessage(const std::shared_ptr<zrpc::TcpConnection>& conn,
                     zrpc::Buffer* buf) {
  if (buf->ReadableBytes() == 0) {
    return;
  }
  conn->Send(buf->Peek(), static_cast<int>(buf->ReadableBytes()));
  buf->RetrieveAll();
}
}  // namespace

int main() {
#ifdef _WIN32
  WSADATA wsa_data{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
    return 1;
  }
#endif

  std::atomic<bool> done{false};
  std::atomic<bool> success{false};

  std::thread server_thread([&]() {
    zrpc::EventLoop loop;
    zrpc::TcpServer server(&loop, "127.0.0.1", static_cast<int16_t>(kPort), nullptr);
    server.SetThreadNum(0);
    server.SetMessageCallback(OnServerMessage);
    server.Start();

    loop.RunAfter(8.0, false, [&]() { loop.Quit(); });
    loop.Run();
    done.store(true);
  });

  zrpc::EventLoop loop;
  zrpc::TcpClient client(&loop, "127.0.0.1", static_cast<int16_t>(kPort), nullptr);

  auto shutdown = [&]() {
    client.Stop();
    loop.Quit();
  };

  client.SetConnectionCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn) {
    if (conn->Connected()) {
      conn->Send(kPing, static_cast<int>(sizeof(kPing) - 1));
      return;
    }
    loop.QueueInLoop(shutdown);
  });

  client.SetMessageCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn,
                                zrpc::Buffer* buf) {
    const size_t msg_len = sizeof(kPing) - 1;
    if (buf->ReadableBytes() < static_cast<int32_t>(msg_len)) {
      return;
    }
    if (std::memcmp(buf->Peek(), kPing, msg_len) == 0) {
      success.store(true);
      conn->Shutdown();
      loop.QueueInLoop(shutdown);
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  client.Connect();
  loop.RunAfter(5.0, false, [&]() { loop.QueueInLoop(shutdown); });
  loop.Run();

  server_thread.join();

#ifdef _WIN32
  WSACleanup();
#endif
  return success.load() ? 0 : 1;
}
