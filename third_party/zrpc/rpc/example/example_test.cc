#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "example_service.h"
#include "echo.zrpc.h"
#include "zrpc/grpc/rpc_service_discovery.h"
#include "zrpc/net/event_loop.h"
#include "zrpc/rpc/client.h"
#include "zrpc/rpc/options.h"
#include "zrpc/rpc/server.h"

namespace {

constexpr uint16_t kTestPort = 16380;
constexpr char kTestIp[] = "127.0.0.1";

class TestServer {
 public:
  void Start() {
    thread_ = std::thread([this]() {
      zrpc::EventLoop loop;
      loop_ = &loop;
      service_ = std::make_unique<rpc_example::EchoRpcHandlerImpl>();
      zrpc::rpc::ServerOptions server_opts;
      server_opts.worker_threads = 4;
      server_ = std::make_unique<zrpc::rpc::Server>(&loop, kTestIp, kTestPort,
                                                    server_opts);
      echo::RegisterEchoRpc(server_.get(), service_.get());
      server_->Start();
      ready_.store(true, std::memory_order_release);
      loop.Run();
      if (server_) {
        server_->PrepareShutdown();
        for (int i = 0; i < 200; ++i) {
          loop.PollOnce(10);
        }
      }
      server_.reset();
      service_.reset();
      loop_ = nullptr;
    });

    while (!ready_.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  void Stop() {
    if (loop_ != nullptr) {
      loop_->QueueInLoop([this]() { loop_->Quit(); });
    }
    if (thread_.joinable()) {
      thread_.join();
    }
  }

 private:
  std::unique_ptr<rpc_example::EchoRpcHandlerImpl> service_;
  std::unique_ptr<zrpc::rpc::Server> server_;
  zrpc::EventLoop* loop_ = nullptr;
  std::thread thread_;
  std::atomic<bool> ready_{false};
};

void DrainLoop(zrpc::EventLoop* loop) {
  loop->RunAfter(0.001, false, [loop]() { loop->Quit(); });
  loop->Run();
}

echo::PingRequest MakeRequest(const std::string& id) {
  echo::PingRequest request;
  request.set_id(id);
  return request;
}

int TestSyncPing() {
  TestServer ts;
  ts.Start();

  zrpc::EventLoop loop;
  zrpc::rpc::Client client(&loop, kTestIp, kTestPort);
  client.Connect(true);
  echo::EchoRpcClient echo(&client);

  echo::PongRepsonse response;
  const zrpc::rpc::Reply reply = echo.Ping(MakeRequest("world"), &response);
  if (!reply.ok() || response.id() != "pong:world") {
    std::cerr << "FAIL TestSyncPing: " << reply.error() << std::endl;
    ts.Stop();
    return 1;
  }

  client.Shutdown();
  DrainLoop(&loop);
  ts.Stop();
  std::cout << "PASS TestSyncPing" << std::endl;
  return 0;
}

int TestEmptyRequestRejected() {
  TestServer ts;
  ts.Start();

  zrpc::EventLoop loop;
  zrpc::rpc::Client client(&loop, kTestIp, kTestPort);
  client.Connect(true);
  echo::EchoRpcClient echo(&client);

  echo::PongRepsonse response;
  const zrpc::rpc::Reply reply = echo.Ping(MakeRequest(""), &response);
  if (reply.ok()) {
    std::cerr << "FAIL TestEmptyRequestRejected" << std::endl;
    ts.Stop();
    return 1;
  }

  client.Shutdown();
  DrainLoop(&loop);
  ts.Stop();
  std::cout << "PASS TestEmptyRequestRejected" << std::endl;
  return 0;
}

int TestServiceFailure() {
  TestServer ts;
  ts.Start();

  zrpc::EventLoop loop;
  zrpc::rpc::Client client(&loop, kTestIp, kTestPort);
  client.Connect(true);
  echo::EchoRpcClient echo(&client);

  echo::PongRepsonse response;
  const zrpc::rpc::Reply reply = echo.Fail(MakeRequest("x"), &response);
  if (reply.ok() || reply.error() != "forced failure") {
    std::cerr << "FAIL TestServiceFailure: " << reply.error() << std::endl;
    ts.Stop();
    return 1;
  }

  client.Shutdown();
  DrainLoop(&loop);
  ts.Stop();
  std::cout << "PASS TestServiceFailure" << std::endl;
  return 0;
}

int TestTimeout() {
  TestServer ts;
  ts.Start();

  zrpc::EventLoop loop;
  zrpc::rpc::ClientOptions options;
  options.retry.max_retries = 0;
  zrpc::rpc::Client client(&loop, kTestIp, kTestPort, options);
  client.Connect(true);
  echo::EchoRpcClient echo(&client);

  echo::PongRepsonse response;
  zrpc::rpc::CallOptions call_opts;
  call_opts.timeout_seconds = 0.05;
  const zrpc::rpc::Reply reply =
      echo.Slow(MakeRequest("x"), &response, call_opts);
  if (reply.ok()) {
    std::cerr << "FAIL TestTimeout" << std::endl;
    ts.Stop();
    return 1;
  }

  client.Shutdown();
  DrainLoop(&loop);
  ts.Stop();
  std::cout << "PASS TestTimeout" << std::endl;
  return 0;
}

int TestAsyncAndShutdown() {
  TestServer ts;
  ts.Start();

  zrpc::EventLoop loop;
  zrpc::rpc::Client client(&loop, kTestIp, kTestPort);
  client.Connect(true);
  echo::EchoRpcClient echo(&client);

  std::atomic<int> done{0};
  constexpr int kTotal = 50;
  for (int i = 0; i < kTotal; ++i) {
    echo.PingAsync(MakeRequest("async"),
                   [&done](const zrpc::rpc::Reply& reply,
                           const echo::PongRepsonse& response) {
                     if (reply.ok() && response.id() == "pong:async") {
                       done.fetch_add(1, std::memory_order_relaxed);
                     }
                   });
  }

  loop.RunAfter(0.001, true, [&done, &loop, kTotal]() {
    if (done.load(std::memory_order_relaxed) >= kTotal) {
      loop.Quit();
    }
  });
  loop.Run();

  client.Shutdown();

  if (done.load(std::memory_order_relaxed) != kTotal) {
    std::cerr << "FAIL TestAsyncAndShutdown done=" << done.load() << std::endl;
    ts.Stop();
    return 1;
  }
  std::cout << "PASS TestAsyncAndShutdown metrics=" << client.MetricsString()
            << std::endl;
  ts.Stop();
  return 0;
}

int TestServiceDiscovery() {
  TestServer ts;
  ts.Start();

  zrpc::EventLoop loop;
  auto discovery = std::make_shared<zrpc::StaticServiceDiscovery>(
      std::vector<zrpc::RpcEndpoint>{zrpc::RpcEndpoint(kTestIp, kTestPort)});
  zrpc::rpc::Client client(&loop, discovery);
  client.Connect(true);
  echo::EchoRpcClient echo(&client);

  echo::PongRepsonse response;
  const zrpc::rpc::Reply reply =
      echo.Ping(MakeRequest("discovered"), &response);
  if (!reply.ok() || response.id() != "pong:discovered") {
    std::cerr << "FAIL TestServiceDiscovery" << std::endl;
    ts.Stop();
    return 1;
  }

  client.Shutdown();
  DrainLoop(&loop);
  ts.Stop();
  std::cout << "PASS TestServiceDiscovery" << std::endl;
  return 0;
}

int TestAsyncRetry() {
  rpc_example::g_flaky_remaining.store(2);
  TestServer ts;
  ts.Start();

  zrpc::EventLoop loop;
  zrpc::rpc::ClientOptions options;
  options.retry.max_retries = 3;
  options.retry.base_backoff_seconds = 0.01;
  zrpc::rpc::Client client(&loop, kTestIp, kTestPort, options);
  client.Connect(true);
  echo::EchoRpcClient echo(&client);

  std::atomic<bool> done{false};
  echo::PongRepsonse final_response;
  zrpc::rpc::Reply final_reply;
  echo.FlakyAsync(MakeRequest("retry-me"),
                  [&](const zrpc::rpc::Reply& reply,
                      const echo::PongRepsonse& response) {
                    final_reply = reply;
                    final_response = response;
                    done.store(true, std::memory_order_release);
                  });

  loop.RunAfter(0.001, true, [&done, &loop]() {
    if (done.load(std::memory_order_acquire)) {
      loop.Quit();
    }
  });
  loop.Run();

  client.Shutdown();

  if (!done.load(std::memory_order_acquire) || !final_reply.ok() ||
      final_response.id() != "recovered:retry-me") {
    std::cerr << "FAIL TestAsyncRetry" << std::endl;
    ts.Stop();
    return 1;
  }

  ts.Stop();
  std::cout << "PASS TestAsyncRetry" << std::endl;
  return 0;
}

int TestWorkerPoolNonBlocking() {
  TestServer ts;
  ts.Start();

  zrpc::EventLoop loop;
  zrpc::rpc::Client client(&loop, kTestIp, kTestPort);
  client.Connect(true);
  echo::EchoRpcClient echo(&client);

  echo.SlowAsync(MakeRequest("blocked"),
                 [](const zrpc::rpc::Reply&, const echo::PongRepsonse&) {});

  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  echo::PongRepsonse response;
  const auto start = std::chrono::steady_clock::now();
  const zrpc::rpc::Reply reply = echo.Ping(MakeRequest("fast"), &response);
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - start)
                              .count();

  client.Shutdown();
  DrainLoop(&loop);
  ts.Stop();

  if (!reply.ok() || response.id() != "pong:fast" || elapsed_ms > 150) {
    std::cerr << "FAIL TestWorkerPoolNonBlocking elapsed_ms=" << elapsed_ms
              << std::endl;
    return 1;
  }
  std::cout << "PASS TestWorkerPoolNonBlocking elapsed_ms=" << elapsed_ms
            << std::endl;
  return 0;
}

}  // namespace

int main() {
  int failed = 0;
  failed += TestSyncPing();
  failed += TestEmptyRequestRejected();
  failed += TestServiceFailure();
  failed += TestTimeout();
  failed += TestAsyncAndShutdown();
  failed += TestServiceDiscovery();
  failed += TestAsyncRetry();
  failed += TestWorkerPoolNonBlocking();

  if (failed == 0) {
    std::cout << "ALL TESTS PASSED" << std::endl;
  }
  return failed;
}
