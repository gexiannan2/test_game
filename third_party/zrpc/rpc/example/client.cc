#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include "echo.zrpc.h"
#include "zrpc/base/logger.h"
#include "zrpc/net/event_loop.h"
#include "zrpc/rpc/client.h"

namespace {

constexpr uint16_t kDefaultPort = 16380;
constexpr int kDefaultRequests = 1000;
constexpr int kDefaultInflight = 32;

struct Config {
  uint16_t port = kDefaultPort;
  int requests = kDefaultRequests;
  int inflight = kDefaultInflight;
};

Config ParseArgs(int argc, char* argv[]) {
  Config cfg;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.rfind("--port=", 0) == 0) {
      cfg.port = static_cast<uint16_t>(std::atoi(arg.substr(7).c_str()));
    } else if (arg.rfind("--requests=", 0) == 0) {
      cfg.requests = std::atoi(arg.substr(11).c_str());
    } else if (arg.rfind("--inflight=", 0) == 0) {
      cfg.inflight = std::atoi(arg.substr(11).c_str());
    }
  }
  if (cfg.inflight < 1) {
    cfg.inflight = 1;
  }
  if (cfg.requests < 1) {
    cfg.requests = 1;
  }
  if (cfg.port == 0) {
    cfg.port = kDefaultPort;
  }
  return cfg;
}

class BenchClient : public std::enable_shared_from_this<BenchClient> {
 public:
  BenchClient(zrpc::EventLoop* loop, uint16_t port, int requests, int inflight)
      : loop_(loop),
        requests_(requests),
        inflight_(inflight),
        client_(loop, "127.0.0.1", port),
        echo_(&client_) {
    client_.EnableRetry();
  }

  void Start() {
    std::weak_ptr<BenchClient> weak = weak_from_this();
    client_.SetConnectionCallback(
        [weak](const std::shared_ptr<zrpc::TcpConnection>& conn) {
          if (auto self = weak.lock()) {
            self->OnConnection(conn);
          }
        });
    client_.Connect();
  }

 private:
  void OnConnection(const std::shared_ptr<zrpc::TcpConnection>& conn) {
    if (!conn->Connected()) {
      if (pending_.load(std::memory_order_relaxed) == 0) {
        loop_->Quit();
      }
      return;
    }
    if (started_.exchange(true)) {
      return;
    }
    IssueMore();
  }

  void OnDone(bool succeeded) {
    if (!succeeded) {
      failed_.fetch_add(1, std::memory_order_relaxed);
    }
    completed_.fetch_add(1, std::memory_order_relaxed);
    pending_.fetch_sub(1, std::memory_order_relaxed);
    if (completed_.load(std::memory_order_relaxed) >= requests_) {
      LOG_INFO << "benchmark done completed=" << completed_.load()
               << " failed=" << failed_.load()
               << " metrics=" << client_.MetricsString();
      loop_->Quit();
      return;
    }
    IssueMore();
  }

  void IssueMore() {
    while (pending_.load(std::memory_order_relaxed) < inflight_ &&
           issued_.load(std::memory_order_relaxed) < requests_) {
      issued_.fetch_add(1, std::memory_order_relaxed);
      pending_.fetch_add(1, std::memory_order_relaxed);

      echo::PingRequest request;
      request.set_id("hello");

      auto self = shared_from_this();
      echo_.PingAsync(request,
                      [self](const zrpc::rpc::Reply& reply,
                             const echo::PongRepsonse& response) {
                        self->OnDone(reply.ok() &&
                                     response.id() == "pong:hello");
                      });
    }
  }

  zrpc::EventLoop* loop_;
  int requests_;
  int inflight_;
  zrpc::rpc::Client client_;
  echo::EchoRpcClient echo_;
  std::atomic<bool> started_{false};
  std::atomic<int> issued_{0};
  std::atomic<int> pending_{0};
  std::atomic<int> completed_{0};
  std::atomic<int> failed_{0};
};

}  // namespace

int main(int argc, char* argv[]) {
  const Config cfg = ParseArgs(argc, argv);
  zrpc::EventLoop loop;
  auto bench = std::make_shared<BenchClient>(&loop, cfg.port, cfg.requests,
                                             cfg.inflight);
  bench->Start();
  loop.Run();
  return 0;
}
