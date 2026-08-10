#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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
constexpr uint16_t kBasePort = 19700;
constexpr int kServerThreads = 4;
constexpr int kConnectBatch = 50;
constexpr double kConnectIntervalSec = 0.01;

using Clock = std::chrono::steady_clock;

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

void DefaultEcho(const std::shared_ptr<zrpc::TcpConnection>& conn, zrpc::Buffer* buf) {
  if (buf->ReadableBytes() == 0) {
    return;
  }
  conn->Send(buf->Peek(), static_cast<int>(buf->ReadableBytes()));
  buf->RetrieveAll();
}

struct ClientSlot {
  std::unique_ptr<zrpc::TcpClient> client;
  std::atomic<bool> connected{false};
  std::atomic<bool> echoed{false};
  std::atomic<bool> disconnected{false};
  int id{0};
  std::string payload;
};

void RampConnect(zrpc::EventLoop* loop, const std::vector<std::shared_ptr<ClientSlot>>& slots) {
  for (size_t i = 0; i < slots.size(); ++i) {
    const double delay = static_cast<double>(i / kConnectBatch) * kConnectIntervalSec;
    auto slot = slots[i];
    loop->RunAfter(delay, false, [slot]() { slot->client->Connect(); });
  }
}

bool WaitUntil(zrpc::EventLoop* loop, const std::function<bool()>& pred, double timeout_sec) {
  const auto deadline =
      Clock::now() +
      std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(timeout_sec));
  while (Clock::now() < deadline) {
    if (pred()) {
      return true;
    }
    if (loop != nullptr) {
      loop->PollOnce(50);
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
  return pred();
}

struct ThroughputConfig {
  int connections;
  int msg_size;
  int pipeline;
  int seconds;
  int server_threads;
  uint64_t min_round_trips;
  int min_connected;
};

bool TestMassConnect(int client_count, int server_threads, double timeout_sec) {
  const uint16_t port = NextPort();
  std::atomic<bool> server_ready{false};
  std::atomic<int> server_connects{0};
  std::atomic<int> server_disconnects{0};

  std::thread server_thread([&]() {
    zrpc::EventLoop loop;
    zrpc::TcpServer server(&loop, kHost, static_cast<int16_t>(port), nullptr);
    server.SetThreadNum(static_cast<int16_t>(server_threads));
    server.SetMessageCallback(DefaultEcho);
    server.SetConnectionCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn) {
      if (conn->Connected()) {
        server_connects.fetch_add(1, std::memory_order_relaxed);
      } else {
        server_disconnects.fetch_add(1, std::memory_order_relaxed);
      }
    });
    server.Start();
    server_ready.store(true, std::memory_order_release);
    loop.RunAfter(timeout_sec + 10.0, false, [&]() { loop.Quit(); });
    loop.Run();
  });

  if (!WaitUntil(nullptr, [&]() { return server_ready.load(std::memory_order_acquire); }, 10.0)) {
    server_thread.join();
    return false;
  }

  zrpc::EventLoop loop;
  std::vector<std::shared_ptr<ClientSlot>> slots;
  slots.reserve(static_cast<size_t>(client_count));

  std::atomic<int> echoed_count{0};

  for (int i = 0; i < client_count; ++i) {
    auto slot = std::make_shared<ClientSlot>();
    slot->id = i;
    slot->payload = "mass-" + std::to_string(i);
    slot->client = std::make_unique<zrpc::TcpClient>(
        &loop, kHost, static_cast<int16_t>(port), nullptr);

    slot->client->SetConnectionCallback([slot](const std::shared_ptr<zrpc::TcpConnection>& conn) {
      if (conn->Connected()) {
        slot->connected.store(true, std::memory_order_relaxed);
        conn->Send(slot->payload.data(), static_cast<int>(slot->payload.size()));
        return;
      }
      slot->disconnected.store(true, std::memory_order_relaxed);
    });

    slot->client->SetMessageCallback(
        [slot, &echoed_count, client_count, &loop](
            const std::shared_ptr<zrpc::TcpConnection>& conn, zrpc::Buffer* buf) {
          if (buf->ReadableBytes() < static_cast<int32_t>(slot->payload.size())) {
            return;
          }
          if (buf->ToStringView().substr(0, slot->payload.size()) == slot->payload) {
            if (!slot->echoed.exchange(true)) {
              const int done = echoed_count.fetch_add(1, std::memory_order_relaxed) + 1;
              if (done >= client_count) {
                loop.QueueInLoop([&loop]() { loop.Quit(); });
              }
            }
            conn->Shutdown();
          }
        });

    slots.push_back(slot);
  }

  RampConnect(&loop, slots);
  loop.RunAfter(timeout_sec, false, [&]() {
    for (auto& slot : slots) {
      slot->client->Stop();
    }
    loop.Quit();
  });
  loop.Run();
  server_thread.join();

  int echoed = echoed_count.load();
  int connected = 0;
  for (const auto& slot : slots) {
    if (slot->connected.load()) {
      ++connected;
    }
  }

  std::cout << "  connected=" << connected << "/" << client_count
            << " echoed=" << echoed << "/" << client_count
            << " server_connects=" << server_connects.load() << '\n';
  return echoed == client_count && server_connects.load() >= client_count;
}

bool TestThroughput(const ThroughputConfig& cfg) {
  const uint16_t port = NextPort();
  std::atomic<bool> server_ready{false};
  std::atomic<uint64_t> server_messages{0};
  std::atomic<uint64_t> server_bytes{0};

  std::thread server_thread([&]() {
    zrpc::EventLoop loop;
    zrpc::TcpServer server(&loop, kHost, static_cast<int16_t>(port), nullptr);
    server.SetThreadNum(static_cast<int16_t>(cfg.server_threads));
    server.SetMessageCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn,
                                  zrpc::Buffer* buf) {
      while (buf->ReadableBytes() >= cfg.msg_size) {
        server_messages.fetch_add(1, std::memory_order_relaxed);
        server_bytes.fetch_add(static_cast<uint64_t>(cfg.msg_size), std::memory_order_relaxed);
        conn->Send(buf->Peek(), cfg.msg_size);
        buf->Retrieve(cfg.msg_size);
      }
    });
    server.Start();
    server_ready.store(true, std::memory_order_release);
    loop.RunAfter(static_cast<double>(cfg.seconds + 60), false, [&]() { loop.Quit(); });
    loop.Run();
  });

  if (!WaitUntil(nullptr, [&]() { return server_ready.load(std::memory_order_acquire); }, 10.0)) {
    server_thread.join();
    return false;
  }

  struct ConnCtx {
    std::unique_ptr<zrpc::TcpClient> client;
    int in_flight{0};
    bool connected{false};
  };

  std::atomic<bool> running{true};
  std::atomic<int> connected_count{0};
  std::atomic<uint64_t> round_trips{0};
  std::atomic<uint64_t> bytes{0};

  std::string payload(static_cast<size_t>(cfg.msg_size), 'x');
  for (int i = 0; i < cfg.msg_size; ++i) {
    payload[static_cast<size_t>(i)] = static_cast<char>('A' + (i % 26));
  }

  zrpc::EventLoop loop;
  std::vector<std::shared_ptr<ConnCtx>> conns;
  conns.reserve(static_cast<size_t>(cfg.connections));

  auto send_one = [&](const std::shared_ptr<zrpc::TcpConnection>& conn, ConnCtx* ctx) {
    conn->Send(payload.data(), static_cast<int>(payload.size()));
    ++ctx->in_flight;
  };

  auto fill_pipeline = [&](const std::shared_ptr<zrpc::TcpConnection>& conn, ConnCtx* ctx) {
    for (int i = 0; i < cfg.pipeline; ++i) {
      send_one(conn, ctx);
    }
  };

  for (int i = 0; i < cfg.connections; ++i) {
    auto ctx = std::make_shared<ConnCtx>();
    ctx->client = std::make_unique<zrpc::TcpClient>(
        &loop, kHost, static_cast<int16_t>(port), nullptr);

    ctx->client->SetConnectionCallback(
        [&, ctx](const std::shared_ptr<zrpc::TcpConnection>& conn) {
          if (conn->Connected()) {
            ctx->connected = true;
            connected_count.fetch_add(1, std::memory_order_relaxed);
            fill_pipeline(conn, ctx.get());
            return;
          }
          if (ctx->connected) {
            ctx->connected = false;
            connected_count.fetch_sub(1, std::memory_order_relaxed);
          }
        });

    ctx->client->SetMessageCallback(
        [&, ctx](const std::shared_ptr<zrpc::TcpConnection>& conn, zrpc::Buffer* buf) {
          while (buf->ReadableBytes() >= cfg.msg_size) {
            buf->Retrieve(cfg.msg_size);
            round_trips.fetch_add(1, std::memory_order_relaxed);
            bytes.fetch_add(static_cast<uint64_t>(cfg.msg_size), std::memory_order_relaxed);
            --ctx->in_flight;
            if (running.load(std::memory_order_relaxed)) {
              send_one(conn, ctx.get());
            }
          }
        });

    conns.push_back(ctx);
  }

  for (size_t i = 0; i < conns.size(); ++i) {
    const double delay = static_cast<double>(i / kConnectBatch) * kConnectIntervalSec;
    auto ctx = conns[i];
    loop.RunAfter(delay, false, [ctx]() { ctx->client->Connect(); });
  }

  const double connect_timeout =
      static_cast<double>(cfg.connections) / kConnectBatch * kConnectIntervalSec + 45.0;
  const bool enough_connected = WaitUntil(
      &loop,
      [&]() { return connected_count.load() >= cfg.min_connected; },
      connect_timeout);

  const auto t0 = Clock::now();
  loop.RunAfter(static_cast<double>(cfg.seconds), false, [&]() {
    running.store(false);
    for (auto& ctx : conns) {
      ctx->client->Stop();
    }
    loop.Quit();
  });
  loop.Run();
  const auto t1 = Clock::now();
  server_thread.join();

  const double elapsed =
      std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0).count();
  const uint64_t rtt = round_trips.load();
  const uint64_t by = bytes.load();
  const double mib = by / elapsed / 1024.0 / 1024.0;

  std::cout << "  connected=" << connected_count.load() << "/" << cfg.connections
            << " enough_connected=" << (enough_connected ? "yes" : "no")
            << " round_trips=" << rtt
            << " msg/s=" << static_cast<uint64_t>(rtt / elapsed)
            << " MiB/s=" << mib << " GiB/s=" << (mib / 1024.0)
            << " server_msgs=" << server_messages.load() << '\n';

  return enough_connected && rtt >= cfg.min_round_trips;
}

bool TestMassChurn(int clients, int rounds) {
  const uint16_t port = NextPort();
  std::atomic<bool> server_ready{false};

  std::thread server_thread([&]() {
    zrpc::EventLoop loop;
    zrpc::TcpServer server(&loop, kHost, static_cast<int16_t>(port), nullptr);
    server.SetThreadNum(static_cast<int16_t>(kServerThreads));
    server.SetMessageCallback(DefaultEcho);
    server.Start();
    server_ready.store(true, std::memory_order_release);
    loop.RunAfter(240.0, false, [&]() { loop.Quit(); });
    loop.Run();
  });

  if (!WaitUntil(nullptr, [&]() { return server_ready.load(std::memory_order_acquire); }, 10.0)) {
    server_thread.join();
    return false;
  }

  struct ChurnClient {
    std::unique_ptr<zrpc::TcpClient> client;
    std::atomic<int> success{0};
    std::atomic<bool> done{false};
    int id{0};
  };

  zrpc::EventLoop loop;
  std::vector<std::shared_ptr<ChurnClient>> slots;
  slots.reserve(static_cast<size_t>(clients));
  std::atomic<int> churn_done{0};

  for (int i = 0; i < clients; ++i) {
    auto slot = std::make_shared<ChurnClient>();
    slot->id = i;
    slot->client = std::make_unique<zrpc::TcpClient>(
        &loop, kHost, static_cast<int16_t>(port), nullptr);
    slot->client->EnableRetry();

    slot->client->SetConnectionCallback(
        [slot](const std::shared_ptr<zrpc::TcpConnection>& conn) {
          if (!conn->Connected()) {
            return;
          }
          const std::string payload =
              "churn-" + std::to_string(slot->id) + "-" + std::to_string(slot->success.load());
          conn->Send(payload.data(), static_cast<int>(payload.size()));
        });

    slot->client->SetMessageCallback(
        [slot, rounds, clients, &churn_done, &loop](
            const std::shared_ptr<zrpc::TcpConnection>& conn, zrpc::Buffer* buf) {
          const std::string expect =
              "churn-" + std::to_string(slot->id) + "-" + std::to_string(slot->success.load());
          if (buf->ReadableBytes() < static_cast<int32_t>(expect.size())) {
            return;
          }
          if (buf->ToStringView().substr(0, expect.size()) != expect) {
            return;
          }
          const int next = slot->success.fetch_add(1) + 1;
          if (next >= rounds) {
            slot->client->CloseRetry();
            slot->done.store(true, std::memory_order_relaxed);
            conn->Shutdown();
            if (churn_done.fetch_add(1, std::memory_order_relaxed) + 1 >= clients) {
              loop.QueueInLoop([&loop]() { loop.Quit(); });
            }
          } else {
            conn->Shutdown();
          }
        });

    slots.push_back(slot);
  }

  for (size_t i = 0; i < slots.size(); ++i) {
    const double delay = static_cast<double>(i / kConnectBatch) * kConnectIntervalSec;
    auto slot = slots[i];
    loop.RunAfter(delay, false, [slot]() { slot->client->Connect(); });
  }

  loop.RunAfter(180.0, false, [&]() {
    for (auto& slot : slots) {
      slot->client->Stop();
    }
    loop.Quit();
  });
  loop.Run();
  server_thread.join();

  int ok = 0;
  for (const auto& slot : slots) {
    if (slot->success.load() == rounds && slot->done.load()) {
      ++ok;
    }
  }
  std::cout << "  churn_ok=" << ok << "/" << clients << '\n';
  return ok == clients;
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
      {"mass_connect_1000_mt4",
       []() { return TestMassConnect(1000, kServerThreads, 120.0); }},
      {"mass_connect_500_mt4",
       []() { return TestMassConnect(500, kServerThreads, 90.0); }},
      {"mass_connect_200_st",
       []() { return TestMassConnect(200, 0, 60.0); }},
      {"throughput_1000conn_1k_p8_15s",
       []() {
         return TestThroughput(
             ThroughputConfig{1000, 1024, 8, 15, kServerThreads, 50000, 200});
       }},
      {"throughput_500conn_4k_p16_15s",
       []() {
         return TestThroughput(
             ThroughputConfig{500, 4096, 16, 15, kServerThreads, 10000, 150});
       }},
      {"throughput_256conn_64k_p8_15s",
       []() {
         return TestThroughput(
             ThroughputConfig{256, 65536, 8, 15, kServerThreads, 5000, 100});
       }},
      {"throughput_100conn_1mb_p4_15s",
       []() {
         return TestThroughput(
             ThroughputConfig{100, 1048576, 4, 15, kServerThreads, 200, 50});
       }},
      {"throughput_64conn_256k_p16_15s",
       []() {
         return TestThroughput(
             ThroughputConfig{64, 262144, 16, 15, kServerThreads, 1000, 32});
       }},
      {"mass_churn_200x5",
       []() { return TestMassChurn(200, 5); }},
  };

  std::cout << "zrpc TCP high-pressure tests"
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
