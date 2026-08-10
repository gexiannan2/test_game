#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
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
constexpr uint16_t kBasePort = 19800;
constexpr int kServerThreads = 4;

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
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      return true;
    }
    Fail(name, "assertion failed");
  } catch (const std::exception& ex) {
    Fail(name, ex.what());
  } catch (...) {
    Fail(name, "unknown exception");
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  return false;
}

void CooldownAfterServer() {
  std::this_thread::sleep_for(std::chrono::seconds(2));
}

void DefaultEcho(const std::shared_ptr<zrpc::TcpConnection>& conn, zrpc::Buffer* buf) {
  if (buf->ReadableBytes() == 0) {
    return;
  }
  conn->Send(buf->Peek(), static_cast<int>(buf->ReadableBytes()));
  buf->RetrieveAll();
}

struct ThreadedEchoServer {
  zrpc::EventLoop loop;
  zrpc::TcpServer server;
  const uint16_t port;
  std::atomic<int> server_connects{0};
  std::atomic<int> server_disconnects{0};

  ThreadedEchoServer(uint16_t p, int threads,
                     zrpc::MessageCallback on_message = DefaultEcho,
                     zrpc::ConnectionCallback on_connection = {})
      : loop(),
        server(&loop, kHost, static_cast<int16_t>(p), nullptr),
        port(p) {
    server.SetThreadNum(static_cast<int16_t>(threads));
    server.SetMessageCallback(std::move(on_message));
    if (on_connection) {
      server.SetConnectionCallback(std::move(on_connection));
    } else {
      server.SetConnectionCallback([this](const std::shared_ptr<zrpc::TcpConnection>& conn) {
        if (conn->Connected()) {
          ++server_connects;
        } else {
          ++server_disconnects;
        }
      });
    }
    server.Start();
  }
};

struct ClientSlot {
  std::unique_ptr<zrpc::TcpClient> client;
  std::atomic<bool> connected{false};
  std::atomic<bool> echoed{false};
  std::atomic<bool> disconnected{false};
  int id{0};
};

using Clock = std::chrono::steady_clock;

bool TestMtConcurrentEcho40() {
  const uint16_t port = NextPort();
  ThreadedEchoServer echo(port, kServerThreads);

  constexpr int kClients = 40;
  std::vector<std::shared_ptr<ClientSlot>> slots;
  slots.reserve(kClients);

  auto all_done = [&slots]() {
    for (const auto& slot : slots) {
      if (!slot->echoed.load() || !slot->disconnected.load()) {
        return false;
      }
    }
    return true;
  };

  auto shutdown = [&]() {
    for (auto& slot : slots) {
      slot->client->Stop();
      slot->client.reset();
    }
    echo.server.Stop([&]() { echo.loop.Quit(); });
  };

  for (int i = 0; i < kClients; ++i) {
    auto slot = std::make_shared<ClientSlot>();
    slot->id = i;
    slot->client = std::make_unique<zrpc::TcpClient>(
        &echo.loop, kHost, static_cast<int16_t>(port), nullptr);

    const std::string payload = "mt40-" + std::to_string(i);

    slot->client->SetConnectionCallback(
        [slot, payload, &echo, &all_done, &shutdown](
            const std::shared_ptr<zrpc::TcpConnection>& conn) {
          if (conn->Connected()) {
            slot->connected = true;
            conn->Send(payload.data(), static_cast<int>(payload.size()));
            return;
          }
          slot->disconnected = true;
          if (all_done()) {
            echo.loop.QueueInLoop(shutdown);
          }
        });

    slot->client->SetMessageCallback(
        [slot, payload, &echo, &all_done, &shutdown](
            const std::shared_ptr<zrpc::TcpConnection>& conn, zrpc::Buffer* buf) {
          if (buf->ReadableBytes() < static_cast<int32_t>(payload.size())) {
            return;
          }
          if (buf->ToStringView().substr(0, payload.size()) == payload) {
            slot->echoed = true;
            conn->Shutdown();
          }
          if (all_done()) {
            echo.loop.QueueInLoop(shutdown);
          }
        });

    slots.push_back(slot);
    slot->client->Connect();
  }

  echo.loop.RunAfter(45.0, false, [&]() { echo.loop.QueueInLoop(shutdown); });
  echo.loop.Run();

  int ok = 0;
  for (const auto& slot : slots) {
    if (slot->echoed.load() && slot->disconnected.load()) {
      ++ok;
    }
  }
  CooldownAfterServer();
  return ok == kClients && echo.server_connects.load() >= kClients;
}

bool TestMtReconnectClients20x5() {
  const uint16_t port = NextPort();
  ThreadedEchoServer echo(port, kServerThreads);

  constexpr int kClients = 20;
  constexpr int kRounds = 5;

  struct ReconnectClient {
    std::unique_ptr<zrpc::TcpClient> client;
    std::atomic<int> success{0};
    std::atomic<bool> done{false};
    int id{0};
  };

  std::vector<std::shared_ptr<ReconnectClient>> clients;
  clients.reserve(kClients);

  auto all_done = [&clients]() {
    for (const auto& rc : clients) {
      if (!rc->done.load()) {
        return false;
      }
    }
    return true;
  };

  auto shutdown = [&]() {
    for (auto& rc : clients) {
      rc->client->Stop();
      rc->client.reset();
    }
    echo.server.Stop([&]() { echo.loop.Quit(); });
  };

  for (int i = 0; i < kClients; ++i) {
    auto rc = std::make_shared<ReconnectClient>();
    rc->id = i;
    rc->client = std::make_unique<zrpc::TcpClient>(
        &echo.loop, kHost, static_cast<int16_t>(port), nullptr);
    rc->client->EnableRetry();

    rc->client->SetConnectionCallback([rc](const std::shared_ptr<zrpc::TcpConnection>& conn) {
      if (!conn->Connected()) {
        return;
      }
      const std::string payload = "rc-" + std::to_string(rc->id) + "-" +
                                  std::to_string(rc->success.load());
      conn->Send(payload.data(), static_cast<int>(payload.size()));
    });

    rc->client->SetMessageCallback([rc, kRounds, &echo, &all_done, &shutdown](
                                        const std::shared_ptr<zrpc::TcpConnection>& conn,
                                        zrpc::Buffer* buf) {
      const std::string expect = "rc-" + std::to_string(rc->id) + "-" +
                                 std::to_string(rc->success.load());
      if (buf->ReadableBytes() < static_cast<int32_t>(expect.size())) {
        return;
      }
      if (buf->ToStringView().substr(0, expect.size()) != expect) {
        return;
      }
      const int next = rc->success.fetch_add(1) + 1;
      if (next >= kRounds) {
        rc->client->CloseRetry();
        rc->done = true;
        conn->Shutdown();
      } else {
        conn->Shutdown();
      }
      if (all_done()) {
        echo.loop.QueueInLoop(shutdown);
      }
    });

    clients.push_back(rc);
    rc->client->Connect();
  }

  echo.loop.RunAfter(90.0, false, [&]() { echo.loop.QueueInLoop(shutdown); });
  echo.loop.Run();

  int ok = 0;
  for (const auto& rc : clients) {
    if (rc->success.load() == kRounds && rc->done.load()) {
      ++ok;
    }
  }
  CooldownAfterServer();
  return ok == kClients;
}

bool TestMtServerForceClose30() {
  const uint16_t port = NextPort();

  std::atomic<int> server_closed{0};
  ThreadedEchoServer echo(
      port, kServerThreads, DefaultEcho,
      [&server_closed](const std::shared_ptr<zrpc::TcpConnection>& conn) {
        if (conn->Connected()) {
          conn->GetLoop()->QueueInLoop([conn]() { conn->ForceClose(); });
          return;
        }
        ++server_closed;
      });

  constexpr int kClients = 30;
  std::atomic<int> client_disconnected{0};

  for (int i = 0; i < kClients; ++i) {
    bool disconnected = false;
    zrpc::TcpClient client(&echo.loop, kHost, static_cast<int16_t>(port), nullptr);

    client.SetConnectionCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn) {
      if (!conn->Connected()) {
        disconnected = true;
      }
    });

    client.Connect();

    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (!disconnected && Clock::now() < deadline) {
      echo.loop.PollOnce(50);
    }
    client.Stop();
    if (disconnected) {
      ++client_disconnected;
    }
  }

  echo.loop.RunAfter(1.0, false, [&]() {
    echo.server.Stop([&]() { echo.loop.Quit(); });
  });
  echo.loop.Run();

  CooldownAfterServer();
  return client_disconnected.load() == kClients && server_closed.load() == kClients;
}

bool TestMtMixedChurn50() {
  const uint16_t port = NextPort();
  ThreadedEchoServer echo(port, kServerThreads);

  constexpr int kClients = 25;
  int finished = 0;

  for (int i = 0; i < kClients; ++i) {
    bool got_echo = false;
    bool disconnected = false;
    zrpc::TcpClient client(&echo.loop, kHost, static_cast<int16_t>(port), nullptr);
    const std::string payload = "churn-" + std::to_string(i);

    client.SetConnectionCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn) {
      if (conn->Connected()) {
        conn->Send(payload.data(), static_cast<int>(payload.size()));
        return;
      }
      disconnected = true;
    });

    client.SetMessageCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn,
                                 zrpc::Buffer* buf) {
      if (buf->ReadableBytes() < static_cast<int32_t>(payload.size())) {
        return;
      }
      if (buf->ToStringView().substr(0, payload.size()) == payload) {
        got_echo = true;
        conn->Shutdown();
      }
    });

    client.Connect();

    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while ((!got_echo || !disconnected) && Clock::now() < deadline) {
      echo.loop.PollOnce(50);
    }
    client.Stop();
    if (got_echo && disconnected) {
      ++finished;
    }
  }

  echo.loop.RunAfter(1.0, false, [&]() {
    echo.server.Stop([&]() { echo.loop.Quit(); });
  });
  echo.loop.Run();

  CooldownAfterServer();
  return finished == kClients && echo.server_connects.load() >= kClients &&
         echo.server_disconnects.load() >= kClients;
}

bool TestMtPipelinedBidirectional() {
  constexpr int kRounds = 50;
  const uint16_t port = NextPort();

  std::atomic<int> server_received{0};
  ThreadedEchoServer echo(
      port, kServerThreads,
      [&](const std::shared_ptr<zrpc::TcpConnection>& conn, zrpc::Buffer* buf) {
        while (buf->ReadableBytes() >= 6) {
          if (buf->Peek()[0] != 'm') {
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
    echo.server.Stop([&]() { echo.loop.Quit(); });
  };

  client.SetConnectionCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn) {
    if (!conn->Connected()) {
      echo.loop.QueueInLoop(shutdown);
      return;
    }
    for (int i = 0; i < kRounds; ++i) {
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
      ++client_received;
    }
    if (client_received >= kRounds) {
      conn->Shutdown();
      echo.loop.QueueInLoop(shutdown);
    }
  });

  client.Connect();
  echo.loop.RunAfter(30.0, false, [&]() { echo.loop.QueueInLoop(shutdown); });
  echo.loop.Run();
  CooldownAfterServer();
  return client_received == kRounds && server_received.load() == kRounds;
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
      {"mt_concurrent_echo_40", TestMtConcurrentEcho40},
      {"mt_reconnect_clients_20x5", TestMtReconnectClients20x5},
      {"mt_server_force_close_30", TestMtServerForceClose30},
      {"mt_mixed_churn_50", TestMtMixedChurn50},
      {"mt_pipelined_bidirectional_50", TestMtPipelinedBidirectional},
  };

  std::cout << "zrpc reactor multithread tests"
#ifdef _WIN32
            << " [IOCP]"
#elif defined(__linux__)
            << " [epoll]"
#else
            << " [poll]"
#endif
            << ", server_threads=" << kServerThreads << ", repeat=" << repeat
            << ", cases=" << cases.size() << '\n';
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
