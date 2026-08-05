#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "zrpc/base/buffer.h"
#include "zrpc/net/event_loop.h"
#include "zrpc/net/socket.h"
#include "zrpc/net/tcp_client.h"
#include "zrpc/net/tcp_connection.h"
#include "zrpc/net/tcp_server.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {
constexpr const char* kHost = "127.0.0.1";
constexpr uint16_t kBasePort = 19300;

std::atomic<int> g_port_counter{0};
std::atomic<int> g_failed{0};
std::atomic<int> g_passed{0};

uint16_t NextPort() {
  return static_cast<uint16_t>(kBasePort + (g_port_counter.fetch_add(1) % 200));
}

void Fail(const char* test, const std::string& reason) {
  ++g_failed;
  std::cerr << "[FAIL] " << test << ": " << reason << '\n';
  std::cerr.flush();
}

void Pass(const char* test) {
  ++g_passed;
  std::cout << "[PASS] " << test << '\n';
  std::cout.flush();
}

bool RunCase(const char* name, const std::function<bool()>& fn) {
  try {
    if (fn()) {
      Pass(name);
      return true;
    }
    Fail(name, "assertion failed");
  } catch (const std::exception& ex) {
    Fail(name, ex.what());
  } catch (...) {
    Fail(name, "unknown exception");
  }
  return false;
}

using MessageHandler = std::function<void(const std::shared_ptr<zrpc::TcpConnection>&,
                                          zrpc::Buffer*)>;
using ConnectionHandler =
    std::function<void(const std::shared_ptr<zrpc::TcpConnection>&)>;

struct EchoServer {
  zrpc::EventLoop loop;
  zrpc::TcpServer server;
  const uint16_t port;

  EchoServer(uint16_t p, MessageHandler on_message, ConnectionHandler on_connection = {})
      : loop(),
        server(&loop, kHost, static_cast<int16_t>(p), nullptr),
        port(p) {
    server.SetThreadNum(0);
    server.SetMessageCallback(std::move(on_message));
    if (on_connection) {
      server.SetConnectionCallback(std::move(on_connection));
    }
    server.Start();
  }
};

void DefaultEcho(const std::shared_ptr<zrpc::TcpConnection>& conn, zrpc::Buffer* buf) {
  if (buf->ReadableBytes() == 0) {
    return;
  }
  conn->Send(buf->Peek(), static_cast<int>(buf->ReadableBytes()));
  buf->RetrieveAll();
}

bool TestBasicEcho() {
  const uint16_t port = NextPort();
  EchoServer echo(port, DefaultEcho);

  bool ok = false;
  zrpc::TcpClient client(&echo.loop, kHost, static_cast<int16_t>(port), nullptr);
  const std::string payload = "iocp-basic-echo";

  auto shutdown = [&]() {
    client.Stop();
    echo.loop.Quit();
  };

  client.SetConnectionCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn) {
    if (conn->Connected()) {
      conn->Send(payload.data(), static_cast<int>(payload.size()));
      return;
    }
    echo.loop.QueueInLoop(shutdown);
  });

  client.SetMessageCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn,
                                zrpc::Buffer* buf) {
    if (buf->ReadableBytes() < static_cast<int32_t>(payload.size())) {
      return;
    }
    if (buf->ToStringView().substr(0, payload.size()) == payload) {
      ok = true;
      conn->Shutdown();
      echo.loop.QueueInLoop(shutdown);
    }
  });

  client.Connect();
  echo.loop.RunAfter(5.0, false, [&]() { echo.loop.QueueInLoop(shutdown); });
  echo.loop.Run();
  return ok;
}

bool TestRawChunkedSend() {
  const uint16_t port = NextPort();
  EchoServer echo(port, DefaultEcho);

  const std::string payload = "chunked-by-one-byte";
  bool ok = false;
  size_t send_offset = 0;

  zrpc::TcpClient client(&echo.loop, kHost, static_cast<int16_t>(port), nullptr);
  std::shared_ptr<zrpc::TcpConnection> conn_holder;
  auto shutdown = [&]() {
    client.Stop();
    echo.loop.Quit();
  };

  std::function<void()> send_next;
  send_next = [&]() {
    if (!conn_holder || send_offset >= payload.size()) {
      return;
    }
    conn_holder->Send(&payload[send_offset], 1);
    ++send_offset;
    if (send_offset < payload.size()) {
      echo.loop.RunAfter(0.0, false, [&]() { send_next(); });
    }
  };

  client.SetConnectionCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn) {
    if (conn->Connected()) {
      conn_holder = conn;
      send_next();
      return;
    }
    echo.loop.QueueInLoop(shutdown);
  });

  client.SetMessageCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn,
                                zrpc::Buffer* buf) {
    if (buf->ReadableBytes() < static_cast<int32_t>(payload.size())) {
      return;
    }
    if (buf->ToStringView().substr(0, payload.size()) == payload) {
      ok = true;
      conn->Shutdown();
      echo.loop.QueueInLoop(shutdown);
    }
  });

  client.Connect();
  echo.loop.RunAfter(10.0, false, [&]() { echo.loop.QueueInLoop(shutdown); });
  echo.loop.Run();
  return ok;
}

bool TestRandomChunkedSend() {
  const uint16_t port = NextPort();
  EchoServer echo(port, DefaultEcho);

  std::string payload(4096, '\0');
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<char>('A' + (i % 26));
  }

  std::vector<int> chunks;
  std::mt19937 rng(42);
  std::uniform_int_distribution<int> chunk_dist(1, 17);
  size_t off = 0;
  while (off < payload.size()) {
    const int n = static_cast<int>(std::min<size_t>(chunk_dist(rng), payload.size() - off));
    chunks.push_back(n);
    off += static_cast<size_t>(n);
  }

  bool ok = false;
  size_t chunk_index = 0;
  size_t chunk_offset = 0;

  zrpc::TcpClient client(&echo.loop, kHost, static_cast<int16_t>(port), nullptr);
  std::shared_ptr<zrpc::TcpConnection> conn_holder;
  auto shutdown = [&]() {
    client.Stop();
    echo.loop.Quit();
  };

  std::function<void()> send_next;
  send_next = [&]() {
    if (!conn_holder || chunk_index >= chunks.size()) {
      return;
    }
    const int n = chunks[chunk_index];
    conn_holder->Send(payload.data() + chunk_offset, n);
    chunk_offset += static_cast<size_t>(n);
    ++chunk_index;
    if (chunk_index < chunks.size()) {
      echo.loop.RunAfter(0.0, false, [&]() { send_next(); });
    }
  };

  client.SetConnectionCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn) {
    if (conn->Connected()) {
      conn_holder = conn;
      send_next();
      return;
    }
    echo.loop.QueueInLoop(shutdown);
  });

  client.SetMessageCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn,
                                zrpc::Buffer* buf) {
    if (buf->ReadableBytes() < static_cast<int32_t>(payload.size())) {
      return;
    }
    if (std::memcmp(buf->Peek(), payload.data(), payload.size()) == 0) {
      ok = true;
      conn->Shutdown();
      echo.loop.QueueInLoop(shutdown);
    }
  });

  client.Connect();
  echo.loop.RunAfter(15.0, false, [&]() { echo.loop.QueueInLoop(shutdown); });
  echo.loop.Run();
  return ok;
}

bool TestLargePayload() {
  const uint16_t port = NextPort();
  EchoServer echo(port, DefaultEcho);

  const size_t kSize = 512 * 1024;
  std::string payload(kSize, '\0');
  for (size_t i = 0; i < kSize; ++i) {
    payload[i] = static_cast<char>(i & 0xFF);
  }

  bool ok = false;
  zrpc::TcpClient client(&echo.loop, kHost, static_cast<int16_t>(port), nullptr);

  auto shutdown = [&]() {
    client.Stop();
    echo.loop.Quit();
  };

  client.SetConnectionCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn) {
    if (conn->Connected()) {
      conn->Send(payload.data(), static_cast<int>(payload.size()));
      return;
    }
    echo.loop.QueueInLoop(shutdown);
  });

  client.SetMessageCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn,
                                zrpc::Buffer* buf) {
    if (buf->ReadableBytes() < static_cast<int32_t>(kSize)) {
      return;
    }
    if (std::memcmp(buf->Peek(), payload.data(), kSize) == 0) {
      ok = true;
      conn->Shutdown();
      echo.loop.QueueInLoop(shutdown);
    }
  });

  client.Connect();
  echo.loop.RunAfter(20.0, false, [&]() { echo.loop.QueueInLoop(shutdown); });
  echo.loop.Run();
  return ok;
}

bool TestPartialReadHook() {
  const uint16_t port = NextPort();
  EchoServer echo(
      port, DefaultEcho,
      [](const std::shared_ptr<zrpc::TcpConnection>& conn) {
        if (!conn->Connected()) {
          return;
        }
        const int32_t fd = conn->GetSockfd();
        conn->SetReadHook([fd](char* buf, size_t len, int* /*save_errno*/) -> ssize_t {
          const int cap = static_cast<int>(std::min(len, size_t{8}));
          return zrpc::socket::Read(fd, buf, cap);
        });
      });

  const std::string payload(300, 'x');
  bool ok = false;
  zrpc::TcpClient client(&echo.loop, kHost, static_cast<int16_t>(port), nullptr);
  auto shutdown = [&]() {
    client.Stop();
    echo.loop.Quit();
  };

  client.SetConnectionCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn) {
    if (conn->Connected()) {
      conn->Send(payload.data(), static_cast<int>(payload.size()));
      return;
    }
    echo.loop.QueueInLoop(shutdown);
  });

  client.SetMessageCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn,
                                zrpc::Buffer* buf) {
    if (buf->ReadableBytes() < static_cast<int32_t>(payload.size())) {
      return;
    }
    if (buf->ToStringView().substr(0, payload.size()) == payload) {
      ok = true;
      conn->Shutdown();
      echo.loop.QueueInLoop(shutdown);
    }
  });

  client.Connect();
  echo.loop.RunAfter(10.0, false, [&]() { echo.loop.QueueInLoop(shutdown); });
  echo.loop.Run();
  return ok;
}

bool TestSplitLengthPrefix() {
  const uint16_t port = NextPort();
  const std::string body = "split-length-prefix-body";

  EchoServer echo(
      port,
      [](const std::shared_ptr<zrpc::TcpConnection>& conn, zrpc::Buffer* buf) {
        while (buf->ReadableBytes() >= static_cast<int32_t>(sizeof(int32_t))) {
          const int32_t len = buf->PeekInt32();
          if (len < 0 || len > 1024 * 1024) {
            return;
          }
          if (buf->ReadableBytes() < static_cast<int32_t>(sizeof(int32_t) + len)) {
            return;
          }
          buf->RetrieveInt32();
          std::string frame = buf->RetrieveAsString(len);
          zrpc::Buffer out;
          out.AppendInt32(static_cast<int32_t>(frame.size()));
          out.Append(frame.data(), static_cast<int32_t>(frame.size()));
          conn->Send(&out);
        }
      });

  bool ok = false;
  zrpc::TcpClient client(&echo.loop, kHost, static_cast<int16_t>(port), nullptr);
  std::shared_ptr<zrpc::TcpConnection> conn_holder;
  auto shutdown = [&]() {
    client.Stop();
    echo.loop.Quit();
  };

  client.SetConnectionCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn) {
    if (conn->Connected()) {
      conn_holder = conn;
      zrpc::Buffer header;
      header.AppendInt32(static_cast<int32_t>(body.size()));
      conn->Send(&header);
      echo.loop.RunAfter(0.02, false, [&]() {
        if (conn_holder) {
          conn_holder->Send(body.data(), static_cast<int>(body.size()));
        }
      });
      return;
    }
    echo.loop.QueueInLoop(shutdown);
  });

  client.SetMessageCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn,
                                zrpc::Buffer* buf) {
    if (buf->ReadableBytes() < static_cast<int32_t>(sizeof(int32_t) + body.size())) {
      return;
    }
    const int32_t len = buf->PeekInt32();
    if (len != static_cast<int32_t>(body.size())) {
      return;
    }
    buf->RetrieveInt32();
    if (buf->RetrieveAsString(len) == body) {
      ok = true;
      conn->Shutdown();
      echo.loop.QueueInLoop(shutdown);
    }
  });

  client.Connect();
  echo.loop.RunAfter(10.0, false, [&]() { echo.loop.QueueInLoop(shutdown); });
  echo.loop.Run();
  return ok;
}

bool TestMultiConnection() {
  constexpr int kClients = 8;
  const uint16_t port = NextPort();
  EchoServer echo(port, DefaultEcho);

  int success = 0;
  int next_id = 0;
  zrpc::TcpClient client(&echo.loop, kHost, static_cast<int16_t>(port), nullptr);

  auto shutdown = [&]() {
    client.Stop();
    echo.loop.Quit();
  };

  std::function<void()> connect_next;
  connect_next = [&]() {
    if (next_id >= kClients) {
      return;
    }
    client.Connect();
  };

  client.SetConnectionCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn) {
    if (conn->Connected()) {
      const std::string payload = "conn-" + std::to_string(next_id);
      ++next_id;
      conn->Send(payload.data(), static_cast<int>(payload.size()));
      return;
    }
    echo.loop.RunAfter(0.01, false, [&]() { connect_next(); });
  });

  client.SetMessageCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn,
                                zrpc::Buffer* buf) {
    const std::string expect = "conn-" + std::to_string(success);
    if (buf->ReadableBytes() < static_cast<int32_t>(expect.size())) {
      return;
    }
    if (buf->ToStringView().substr(0, expect.size()) == expect) {
      ++success;
      conn->Shutdown();
      if (success >= kClients) {
        echo.loop.QueueInLoop(shutdown);
      }
    }
  });

  echo.loop.RunAfter(0.05, false, [&]() { connect_next(); });
  echo.loop.RunAfter(20.0, false, [&]() { echo.loop.QueueInLoop(shutdown); });
  echo.loop.Run();
  return success == kClients;
}

bool TestPipelined() {
  constexpr int kMessages = 20;
  const uint16_t port = NextPort();
  EchoServer echo(port, DefaultEcho);

  int received = 0;
  zrpc::TcpClient client(&echo.loop, kHost, static_cast<int16_t>(port), nullptr);

  auto shutdown = [&]() {
    client.Stop();
    echo.loop.Quit();
  };

  client.SetConnectionCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn) {
    if (!conn->Connected()) {
      echo.loop.QueueInLoop(shutdown);
      return;
    }
    for (int i = 0; i < kMessages; ++i) {
      char msg[6];
      std::snprintf(msg, sizeof(msg), "m%04d", i);
      conn->Send(msg, 6);
    }
  });

  client.SetMessageCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn,
                                zrpc::Buffer* buf) {
    while (buf->ReadableBytes() >= 6) {
      if (buf->Peek()[0] != 'm') {
        break;
      }
      buf->Retrieve(6);
      ++received;
    }
    if (received >= kMessages) {
      conn->Shutdown();
      echo.loop.QueueInLoop(shutdown);
    }
  });

  client.Connect();
  echo.loop.RunAfter(10.0, false, [&]() { echo.loop.QueueInLoop(shutdown); });
  echo.loop.Run();
  return received == kMessages;
}

bool TestRapidReconnect() {
  constexpr int kRounds = 20;
  const uint16_t port = NextPort();
  EchoServer echo(port, DefaultEcho);

  int success = 0;
  zrpc::TcpClient client(&echo.loop, kHost, static_cast<int16_t>(port), nullptr);
  client.EnableRetry();

  auto shutdown = [&]() {
    client.Stop();
    echo.loop.Quit();
  };

  client.SetConnectionCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn) {
    if (!conn->Connected()) {
      if (success >= kRounds) {
        client.CloseRetry();
        client.Stop();
      }
      return;
    }
    const std::string payload = "rapid-" + std::to_string(success);
    conn->Send(payload.data(), static_cast<int>(payload.size()));
  });

  client.SetMessageCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn,
                                zrpc::Buffer* buf) {
    const std::string expect = "rapid-" + std::to_string(success);
    if (buf->ReadableBytes() < static_cast<int32_t>(expect.size())) {
      return;
    }
    if (buf->ToStringView().substr(0, expect.size()) == expect) {
      ++success;
      if (success >= kRounds) {
        conn->Shutdown();
        echo.loop.QueueInLoop(shutdown);
        return;
      }
      conn->Shutdown();
    }
  });

  client.Connect();
  echo.loop.RunAfter(30.0, false, [&]() { echo.loop.QueueInLoop(shutdown); });
  echo.loop.Run();
  return success == kRounds;
}

bool TestManySmallPackets() {
  const uint16_t port = NextPort();
  EchoServer echo(port, DefaultEcho);

  constexpr int kPackets = 500;
  std::string payload;
  payload.reserve(static_cast<size_t>(kPackets) * 4);
  for (int i = 0; i < kPackets; ++i) {
    payload.append(std::to_string(i));
    payload.push_back('|');
  }

  std::vector<int> chunks;
  for (size_t off = 0; off < payload.size(); off += 3) {
    chunks.push_back(static_cast<int>(std::min<size_t>(3, payload.size() - off)));
  }

  bool ok = false;
  size_t chunk_index = 0;
  size_t chunk_offset = 0;
  zrpc::TcpClient client(&echo.loop, kHost, static_cast<int16_t>(port), nullptr);
  std::shared_ptr<zrpc::TcpConnection> conn_holder;
  auto shutdown = [&]() {
    client.Stop();
    echo.loop.Quit();
  };

  std::function<void()> send_next;
  send_next = [&]() {
    if (!conn_holder || chunk_index >= chunks.size()) {
      return;
    }
    const int n = chunks[chunk_index];
    conn_holder->Send(payload.data() + chunk_offset, n);
    chunk_offset += static_cast<size_t>(n);
    ++chunk_index;
    if (chunk_index < chunks.size()) {
      echo.loop.RunAfter(0.0, false, [&]() { send_next(); });
    }
  };

  client.SetConnectionCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn) {
    if (conn->Connected()) {
      conn_holder = conn;
      send_next();
      return;
    }
    echo.loop.QueueInLoop(shutdown);
  });

  client.SetMessageCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn,
                                zrpc::Buffer* buf) {
    if (buf->ReadableBytes() < static_cast<int32_t>(payload.size())) {
      return;
    }
    if (std::memcmp(buf->Peek(), payload.data(), payload.size()) == 0) {
      ok = true;
      conn->Shutdown();
      echo.loop.QueueInLoop(shutdown);
    }
  });

  client.Connect();
  echo.loop.RunAfter(15.0, false, [&]() { echo.loop.QueueInLoop(shutdown); });
  echo.loop.Run();
  return ok;
}

bool TestConcurrentBidirectional() {
  constexpr int kRounds = 30;
  const uint16_t port = NextPort();

  int server_received = 0;
  EchoServer echo(
      port,
      [&](const std::shared_ptr<zrpc::TcpConnection>& conn, zrpc::Buffer* buf) {
        while (buf->ReadableBytes() >= 6) {
          if (buf->Peek()[0] != 'b') {
            break;
          }
          conn->Send(buf->Peek(), 6);
          buf->Retrieve(6);
          ++server_received;
        }
      });

  int client_received = 0;
  zrpc::TcpClient client(&echo.loop, kHost, static_cast<int16_t>(port), nullptr);

  auto shutdown = [&]() {
    client.Stop();
    echo.loop.Quit();
  };

  client.SetConnectionCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn) {
    if (!conn->Connected()) {
      echo.loop.QueueInLoop(shutdown);
      return;
    }
    for (int i = 0; i < kRounds; ++i) {
      char msg[6];
      std::snprintf(msg, sizeof(msg), "b%04d", i);
      conn->Send(msg, 6);
    }
  });

  client.SetMessageCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn,
                                zrpc::Buffer* buf) {
    while (buf->ReadableBytes() >= 6) {
      if (buf->Peek()[0] != 'b') {
        break;
      }
      buf->Retrieve(6);
      ++client_received;
    }
    if (client_received >= kRounds) {
      conn->Shutdown();
      echo.loop.QueueInLoop(shutdown);
    }
  });

  client.Connect();
  echo.loop.RunAfter(10.0, false, [&]() { echo.loop.QueueInLoop(shutdown); });
  echo.loop.Run();
  return client_received == kRounds && server_received == kRounds;
}

}  // namespace

int main(int argc, char* argv[]) {
#ifdef _WIN32
  WSADATA wsa_data{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
    return 1;
  }
#endif

  int repeat = 1;
  if (argc > 1) {
    repeat = std::max(1, std::atoi(argv[1]));
  }

  struct Case {
    const char* name;
    std::function<bool()> fn;
  };

  const std::vector<Case> cases = {
      {"basic_echo", TestBasicEcho},
      {"raw_chunked_1byte", TestRawChunkedSend},
      {"random_chunked_send", TestRandomChunkedSend},
      {"large_payload_512k", TestLargePayload},
      {"partial_read_hook_8bytes", TestPartialReadHook},
      {"split_length_prefix", TestSplitLengthPrefix},
      {"multi_connection_8", TestMultiConnection},
      {"pipelined_20", TestPipelined},
      {"concurrent_bidirectional", TestConcurrentBidirectional},
      {"many_small_packets_500", TestManySmallPackets},
      {"rapid_reconnect_20", TestRapidReconnect},
  };

  std::cout << "zrpc TCP reactor stress tests"
#ifdef _WIN32
            << " [IOCP]"
#elif defined(__linux__)
            << " [epoll]"
#else
            << " [poll]"
#endif
            << ", repeat=" << repeat << ", cases=" << cases.size() << '\n';
  std::cout.flush();

  for (int r = 0; r < repeat; ++r) {
    if (repeat > 1) {
      std::cout << "--- round " << (r + 1) << '/' << repeat << " ---\n";
      std::cout.flush();
    }
    for (const auto& c : cases) {
      RunCase(c.name, c.fn);
    }
  }

  std::cout << "summary: passed=" << g_passed.load() << " failed=" << g_failed.load()
            << '\n';
  std::cout.flush();

#ifdef _WIN32
  WSACleanup();
#endif
  return g_failed.load() == 0 ? 0 : 1;
}
