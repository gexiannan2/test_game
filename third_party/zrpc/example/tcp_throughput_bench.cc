#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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
constexpr int kDefaultPort = 19500;
constexpr int kDefaultSeconds = 10;
constexpr int kDefaultMsgSize = 65536;
constexpr int kDefaultPipeline = 4;
constexpr int kDefaultConnections = 1;
constexpr int kDefaultServerThreads = 0;
constexpr int kConnectBatch = 50;
constexpr double kConnectIntervalSec = 0.01;

using Clock = std::chrono::steady_clock;

struct BenchConfig {
  std::string mode = "client";
  std::string host = kHost;
  int port = kDefaultPort;
  int seconds = kDefaultSeconds;
  int msg_size = kDefaultMsgSize;
  int pipeline = kDefaultPipeline;
  int connections = kDefaultConnections;
  int server_threads = kDefaultServerThreads;
};

void PrintUsage(const char* prog) {
  std::cerr
      << "Usage:\n"
      << "  " << prog << " server [port] [msg_size] [seconds] [thread_num]\n"
      << "  " << prog
      << " client [host] [port] [seconds] [msg_size] [pipeline] [connections]\n"
      << "\nDefaults: port=" << kDefaultPort << " seconds=" << kDefaultSeconds
      << " msg_size=" << kDefaultMsgSize << " pipeline=" << kDefaultPipeline
      << " connections=" << kDefaultConnections << " server_threads="
      << kDefaultServerThreads << '\n';
}

bool ParseConfig(int argc, char* argv[], BenchConfig* cfg) {
  if (argc < 2) {
    return false;
  }
  cfg->mode = argv[1];
  if (cfg->mode == "server") {
    if (argc > 2) {
      cfg->port = std::atoi(argv[2]);
    }
    if (argc > 3) {
      cfg->msg_size = std::atoi(argv[3]);
    }
    if (argc > 4) {
      cfg->seconds = std::atoi(argv[4]);
    }
    if (argc > 5) {
      cfg->server_threads = std::atoi(argv[5]);
    }
    return cfg->port > 0 && cfg->msg_size > 0 && cfg->seconds > 0 &&
           cfg->server_threads >= 0;
  }
  if (cfg->mode == "client") {
    if (argc > 2) {
      cfg->host = argv[2];
    }
    if (argc > 3) {
      cfg->port = std::atoi(argv[3]);
    }
    if (argc > 4) {
      cfg->seconds = std::atoi(argv[4]);
    }
    if (argc > 5) {
      cfg->msg_size = std::atoi(argv[5]);
    }
    if (argc > 6) {
      cfg->pipeline = std::atoi(argv[6]);
    }
    if (argc > 7) {
      cfg->connections = std::atoi(argv[7]);
    }
    return cfg->port > 0 && cfg->seconds > 0 && cfg->msg_size > 0 &&
           cfg->pipeline > 0 && cfg->connections > 0;
  }
  return false;
}

void RunServer(const BenchConfig& cfg) {
  std::atomic<uint64_t> messages{0};
  std::atomic<uint64_t> bytes{0};
  std::atomic<int> active_connections{0};

  std::thread server_thread([&]() {
    zrpc::EventLoop loop;
    zrpc::TcpServer server(&loop, kHost, static_cast<int16_t>(cfg.port), nullptr);
    server.SetThreadNum(static_cast<int16_t>(cfg.server_threads));
    server.SetConnectionCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn) {
      if (conn->Connected()) {
        active_connections.fetch_add(1, std::memory_order_relaxed);
      } else {
        active_connections.fetch_sub(1, std::memory_order_relaxed);
      }
    });
    server.SetMessageCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn,
                                  zrpc::Buffer* buf) {
      while (buf->ReadableBytes() >= cfg.msg_size) {
        messages.fetch_add(1, std::memory_order_relaxed);
        bytes.fetch_add(static_cast<uint64_t>(cfg.msg_size), std::memory_order_relaxed);
        conn->Send(buf->Peek(), cfg.msg_size);
        buf->Retrieve(cfg.msg_size);
      }
    });
    server.Start();
    loop.RunAfter(static_cast<double>(cfg.seconds), false, [&]() { loop.Quit(); });
    loop.Run();
  });

  server_thread.join();

  const double sec = static_cast<double>(cfg.seconds);
  const uint64_t msg = messages.load();
  const uint64_t by = bytes.load();
  std::cout << "[server] connections=" << active_connections.load()
            << " threads=" << cfg.server_threads << " msg_size=" << cfg.msg_size
            << '\n';
  std::cout << "[server] messages=" << msg << " msg/s=" << static_cast<uint64_t>(msg / sec)
            << " MiB/s=" << (by / sec / 1024.0 / 1024.0)
            << " GiB/s=" << (by / sec / 1024.0 / 1024.0 / 1024.0) << '\n';
}

void RunClient(const BenchConfig& cfg) {
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

  auto send_one = [&](const std::shared_ptr<zrpc::TcpConnection>& conn,
                      ConnCtx* ctx) {
    conn->Send(payload.data(), static_cast<int>(payload.size()));
    ++ctx->in_flight;
  };

  auto fill_pipeline = [&](const std::shared_ptr<zrpc::TcpConnection>& conn,
                           ConnCtx* ctx) {
    for (int i = 0; i < cfg.pipeline; ++i) {
      send_one(conn, ctx);
    }
  };

  for (int i = 0; i < cfg.connections; ++i) {
    auto ctx = std::make_shared<ConnCtx>();
    ctx->client = std::make_unique<zrpc::TcpClient>(
        &loop, cfg.host.c_str(), static_cast<int16_t>(cfg.port), nullptr);

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
        [&, ctx](const std::shared_ptr<zrpc::TcpConnection>& conn,
                 zrpc::Buffer* buf) {
          while (buf->ReadableBytes() >= cfg.msg_size) {
            buf->Retrieve(cfg.msg_size);
            round_trips.fetch_add(1, std::memory_order_relaxed);
            bytes.fetch_add(static_cast<uint64_t>(cfg.msg_size),
                            std::memory_order_relaxed);
            --ctx->in_flight;

            if (running.load(std::memory_order_relaxed)) {
              send_one(conn, ctx.get());
            }
          }
        });

    conns.push_back(ctx);
  }

  for (size_t i = 0; i < conns.size(); ++i) {
    const double delay = static_cast<double>((i / kConnectBatch) * kConnectBatch) *
                         kConnectIntervalSec / static_cast<double>(kConnectBatch);
    auto ctx = conns[i];
    loop.RunAfter(delay, false, [ctx]() { ctx->client->Connect(); });
  }

  const double connect_timeout =
      static_cast<double>(cfg.connections) / kConnectBatch * kConnectIntervalSec + 30.0;
  const auto connect_deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                                   std::chrono::duration<double>(connect_timeout));
  while (connected_count.load() < cfg.connections && Clock::now() < connect_deadline) {
    loop.PollOnce(50);
  }

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

  const double elapsed =
      std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0).count();
  const uint64_t rtt = round_trips.load();
  const uint64_t by = bytes.load();
  const double mib = by / elapsed / 1024.0 / 1024.0;

  std::cout << "[client] connections=" << cfg.connections
            << " connected=" << connected_count.load()
            << " pipeline=" << cfg.pipeline << " msg_size=" << cfg.msg_size
            << " duration=" << elapsed << "s\n";
  std::cout << "[client] round_trips=" << rtt
            << " total_msg/s=" << static_cast<uint64_t>(rtt / elapsed)
            << " per_conn_msg/s=" << static_cast<uint64_t>(rtt / elapsed / cfg.connections)
            << '\n';
  std::cout << "[client] one_way_MiB/s=" << mib << " one_way_GiB/s=" << (mib / 1024.0)
            << " wire_echo_GiB/s~=" << (mib * 2.0 / 1024.0) << '\n';
}

}  // namespace

int main(int argc, char* argv[]) {
#ifdef _WIN32
  WSADATA wsa_data{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
    return 1;
  }
#endif

  BenchConfig cfg;
  if (!ParseConfig(argc, argv, &cfg)) {
    PrintUsage(argv[0]);
#ifdef _WIN32
    WSACleanup();
#endif
    return 1;
  }

  std::cout << "zrpc tcp throughput bench"
#ifdef _WIN32
            << " [IOCP]"
#elif defined(__linux__)
            << " [epoll]"
#else
            << " [poll]"
#endif
            << " mode=" << cfg.mode << '\n';
  std::cout.flush();

  if (cfg.mode == "server") {
    RunServer(cfg);
  } else {
    RunClient(cfg);
  }

#ifdef _WIN32
  WSACleanup();
#endif
  return 0;
}
