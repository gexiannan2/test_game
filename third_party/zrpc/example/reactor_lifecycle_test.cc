#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "zrpc/base/buffer.h"
#include "zrpc/net/event_loop.h"
#include "zrpc/net/tcp_client.h"
#include "zrpc/net/tcp_connection.h"
#include "zrpc/net/tcp_server.h"

#ifdef _WIN32
#include <winsock2.h>
#endif

namespace {
constexpr const char* kHost = "127.0.0.1";
constexpr uint16_t kBasePort = 19500;

std::atomic<int> g_port_counter{0};
std::atomic<int> g_failed{0};
std::atomic<int> g_passed{0};

uint16_t NextPort() {
  return static_cast<uint16_t>(kBasePort + (g_port_counter.fetch_add(1) % 300));
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

void DefaultEcho(const std::shared_ptr<zrpc::TcpConnection>& conn, zrpc::Buffer* buf) {
  if (buf->ReadableBytes() == 0) {
    return;
  }
  conn->Send(buf->Peek(), static_cast<int>(buf->ReadableBytes()));
  buf->RetrieveAll();
}

struct EchoServer {
  zrpc::EventLoop loop;
  zrpc::TcpServer server;
  const uint16_t port;

  EchoServer(uint16_t p) : loop(), server(&loop, kHost, static_cast<int16_t>(p), nullptr), port(p) {
    server.SetThreadNum(0);
    server.SetMessageCallback(DefaultEcho);
    server.Start();
  }
};

bool RunRetryEchoRounds(int rounds, const char* tag_prefix, double timeout_sec) {
  const uint16_t port = NextPort();
  EchoServer echo(port);

  int success = 0;
  zrpc::TcpClient client(&echo.loop, kHost, static_cast<int16_t>(port), nullptr);
  client.EnableRetry();

  auto shutdown = [&]() {
    client.Stop();
    echo.loop.Quit();
  };

  client.SetConnectionCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn) {
    if (!conn->Connected()) {
      return;
    }
    const std::string payload = std::string(tag_prefix) + std::to_string(success);
    conn->Send(payload.data(), static_cast<int>(payload.size()));
  });

  client.SetMessageCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn,
                                zrpc::Buffer* buf) {
    const std::string expect = std::string(tag_prefix) + std::to_string(success);
    if (buf->ReadableBytes() < static_cast<int32_t>(expect.size())) {
      return;
    }
    if (buf->ToStringView().substr(0, expect.size()) != expect) {
      return;
    }
    ++success;
    if (success >= rounds) {
      client.CloseRetry();
      conn->Shutdown();
      echo.loop.QueueInLoop(shutdown);
      return;
    }
    conn->Shutdown();
  });

  client.Connect();
  echo.loop.RunAfter(timeout_sec, false, [&]() { echo.loop.QueueInLoop(shutdown); });
  echo.loop.Run();
  return success == rounds;
}

bool TestRecursiveReconnect50() { return RunRetryEchoRounds(50, "r50-", 60.0); }

bool TestRecursiveReconnect100() { return RunRetryEchoRounds(100, "r100-", 120.0); }

bool TestManualReconnect20() {
  constexpr int kRounds = 20;
  const uint16_t port = NextPort();
  EchoServer echo(port);

  int success = 0;
  zrpc::TcpClient client(&echo.loop, kHost, static_cast<int16_t>(port), nullptr);

  auto shutdown = [&]() {
    client.Stop();
    echo.loop.Quit();
  };

  client.SetConnectionCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn) {
    if (conn->Connected()) {
      const std::string payload = "manual-" + std::to_string(success);
      conn->Send(payload.data(), static_cast<int>(payload.size()));
      return;
    }
    if (success < kRounds) {
      echo.loop.QueueInLoop([&]() { client.Connect(); });
    }
  });

  client.SetMessageCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn,
                                zrpc::Buffer* buf) {
    const std::string expect = "manual-" + std::to_string(success);
    if (buf->ReadableBytes() < static_cast<int32_t>(expect.size())) {
      return;
    }
    if (buf->ToStringView().substr(0, expect.size()) != expect) {
      return;
    }
    ++success;
    if (success >= kRounds) {
      conn->Shutdown();
      echo.loop.QueueInLoop(shutdown);
      return;
    }
    conn->Shutdown();
  });

  client.Connect();
  echo.loop.RunAfter(30.0, false, [&]() { echo.loop.QueueInLoop(shutdown); });
  echo.loop.Run();
  return success == kRounds;
}

bool TestServerForceClose() {
  const uint16_t port = NextPort();
  EchoServer echo(port);

  bool disconnected = false;
  bool saw_connected = false;
  zrpc::TcpClient client(&echo.loop, kHost, static_cast<int16_t>(port), nullptr);

  auto shutdown = [&]() {
    client.Stop();
    echo.loop.Quit();
  };

  echo.server.SetConnectionCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn) {
    if (conn->Connected()) {
      echo.loop.QueueInLoop([conn]() { conn->ForceClose(); });
    }
  });

  client.SetConnectionCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn) {
    if (conn->Connected()) {
      saw_connected = true;
      conn->Send("ping", 4);
      return;
    }
    disconnected = true;
    echo.loop.QueueInLoop(shutdown);
  });

  client.Connect();
  echo.loop.RunAfter(10.0, false, [&]() { echo.loop.QueueInLoop(shutdown); });
  echo.loop.Run();
  return saw_connected && disconnected;
}

bool TestClientForceClose() {
  const uint16_t port = NextPort();
  EchoServer echo(port);

  bool disconnected = false;
  zrpc::TcpClient client(&echo.loop, kHost, static_cast<int16_t>(port), nullptr);

  auto shutdown = [&]() {
    client.Stop();
    echo.loop.Quit();
  };

  client.SetConnectionCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn) {
    if (conn->Connected()) {
      conn->ForceClose();
      return;
    }
    disconnected = true;
    echo.loop.QueueInLoop(shutdown);
  });

  client.Connect();
  echo.loop.RunAfter(10.0, false, [&]() { echo.loop.QueueInLoop(shutdown); });
  echo.loop.Run();
  return disconnected;
}

bool TestDoubleShutdown() {
  const uint16_t port = NextPort();
  EchoServer echo(port);

  int shutdown_count = 0;
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
    conn->Shutdown();
    conn->Shutdown();
    ++shutdown_count;
  });

  client.Connect();
  echo.loop.RunAfter(10.0, false, [&]() { echo.loop.QueueInLoop(shutdown); });
  echo.loop.Run();
  return shutdown_count == 1;
}

bool TestConnectRefused() {
  const uint16_t port = NextPort();
  zrpc::EventLoop loop;
  bool error_cb_called = false;

  zrpc::TcpClient client(&loop, kHost, static_cast<int16_t>(port), nullptr);
  client.SetConnectionErrorCallBack([&]() {
    error_cb_called = true;
    client.Stop();
    loop.Quit();
  });
  client.CloseRetry();

  client.Connect();
  loop.RunAfter(5.0, false, [&]() {
    client.Stop();
    loop.Quit();
  });
  loop.Run();
  return error_cb_called;
}

bool TestClientRecreate10() {
  const uint16_t port = NextPort();
  EchoServer echo(port);

  int total_ok = 0;
  for (int i = 0; i < 10; ++i) {
    bool ok = false;
    bool done = false;
    zrpc::TcpClient client(&echo.loop, kHost, static_cast<int16_t>(port), nullptr);

    client.SetConnectionCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn) {
      if (conn->Connected()) {
        conn->Send("x", 1);
        return;
      }
      done = true;
    });

    client.SetMessageCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn,
                                  zrpc::Buffer* buf) {
      if (buf->ReadableBytes() >= 1) {
        ok = true;
        conn->Shutdown();
      }
    });

    client.Connect();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((!done || !ok) && std::chrono::steady_clock::now() < deadline) {
      echo.loop.PollOnce(100);
    }
    client.Stop();
    if (ok && done) {
      ++total_ok;
    }
  }
  return total_ok == 10;
}

bool TestSequentialServerConnections() {
  const uint16_t port = NextPort();
  EchoServer echo(port);

  int server_connects = 0;
  int server_disconnects = 0;

  echo.server.SetConnectionCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn) {
    if (conn->Connected()) {
      ++server_connects;
      return;
    }
    ++server_disconnects;
  });

  constexpr int kClients = 15;
  int finished = 0;

  for (int i = 0; i < kClients; ++i) {
    zrpc::TcpClient client(&echo.loop, kHost, static_cast<int16_t>(port), nullptr);
    bool done = false;
    bool got_echo = false;

    client.SetConnectionCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn) {
      if (conn->Connected()) {
        conn->Send("a", 1);
        return;
      }
      done = true;
    });

    client.SetMessageCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn,
                                  zrpc::Buffer* buf) {
      if (buf->ReadableBytes() >= 1) {
        got_echo = true;
        conn->Shutdown();
      }
    });

    client.Connect();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while ((!done || !got_echo) && std::chrono::steady_clock::now() < deadline) {
      echo.loop.PollOnce(100);
    }
    client.Stop();
    if (done && got_echo) {
      ++finished;
    }
  }

  return finished == kClients && server_connects >= kClients &&
         server_disconnects >= kClients;
}

bool TestReconnectWithTimer() {
  constexpr int kRounds = 30;
  const uint16_t port = NextPort();
  EchoServer echo(port);

  int timer_ticks = 0;
  int success = 0;

  zrpc::TcpClient client(&echo.loop, kHost, static_cast<int16_t>(port), nullptr);
  client.EnableRetry();

  auto shutdown = [&]() {
    client.Stop();
    echo.loop.Quit();
  };

  echo.loop.RunAfter(0.01, true, [&]() {
    ++timer_ticks;
    if (success >= kRounds && timer_ticks >= 5) {
      echo.loop.QueueInLoop(shutdown);
    }
  });

  client.SetConnectionCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn) {
    if (!conn->Connected()) {
      return;
    }
    const std::string payload = "tm-" + std::to_string(success);
    conn->Send(payload.data(), static_cast<int>(payload.size()));
  });

  client.SetMessageCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn,
                                zrpc::Buffer* buf) {
    const std::string expect = "tm-" + std::to_string(success);
    if (buf->ReadableBytes() < static_cast<int32_t>(expect.size())) {
      return;
    }
    if (buf->ToStringView().substr(0, expect.size()) != expect) {
      return;
    }
    ++success;
    if (success >= kRounds) {
      client.CloseRetry();
      conn->Shutdown();
      if (timer_ticks >= 5) {
        echo.loop.QueueInLoop(shutdown);
      }
      return;
    }
    conn->Shutdown();
  });

  client.Connect();
  echo.loop.RunAfter(60.0, false, [&]() { echo.loop.QueueInLoop(shutdown); });
  echo.loop.Run();
  return success == kRounds && timer_ticks >= 5;
}

}  // namespace

int main(int argc, char* argv[]) {
#ifdef _WIN32
  WSADATA wsa_data{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
    return 1;
  }
#endif

  g_passed = 0;
  g_failed = 0;

  int repeat = 1;
  const char* filter = nullptr;
  for (int i = 1; i < argc; ++i) {
    const char* arg = argv[i];
    if (std::strcmp(arg, "--filter") == 0 && i + 1 < argc) {
      filter = argv[++i];
      continue;
    }
    if (arg[0] >= '0' && arg[0] <= '9') {
      repeat = std::max(1, std::atoi(arg));
    }
  }

  struct Case {
    const char* name;
    std::function<bool()> fn;
  };

  const std::vector<Case> cases = {
      {"recursive_reconnect_50", TestRecursiveReconnect50},
      {"recursive_reconnect_100", TestRecursiveReconnect100},
      {"manual_reconnect_20", TestManualReconnect20},
      {"server_force_close", TestServerForceClose},
      {"client_force_close", TestClientForceClose},
      {"double_shutdown", TestDoubleShutdown},
      {"connect_refused", TestConnectRefused},
      {"client_recreate_10", TestClientRecreate10},
      {"sequential_server_connections_15", TestSequentialServerConnections},
      {"reconnect_with_timer_30", TestReconnectWithTimer},
  };

  std::cout << "zrpc reactor lifecycle tests"
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
      if (filter != nullptr && std::string(c.name).find(filter) == std::string::npos) {
        continue;
      }
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
