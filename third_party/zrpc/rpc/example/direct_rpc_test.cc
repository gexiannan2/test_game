#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include "direct_protocol.h"
#include "zrpc/net/event_loop.h"
#include "zrpc/rpc/client.h"
#include "zrpc/rpc/server.h"

namespace {

constexpr uint16_t kTestPort = 16381;
constexpr char kTestIp[] = "127.0.0.1";

class DirectTestServer {
 public:
  void Start() {
    thread_ = std::thread([this]() {
      zrpc::EventLoop loop;
      loop_ = &loop;
      zrpc::rpc::ServerOptions server_opts;
      server_opts.worker_threads = 2;
      server_ = std::make_unique<zrpc::rpc::Server>(&loop, kTestIp, kTestPort,
                                                    server_opts);
      direct_example::RegisterDirectProtocols(server_.get());
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
  std::unique_ptr<zrpc::rpc::Server> server_;
  zrpc::EventLoop* loop_ = nullptr;
  std::thread thread_;
  std::atomic<bool> ready_{false};
};

void DrainLoop(zrpc::EventLoop* loop) {
  loop->RunAfter(0.001, false, [loop]() { loop->Quit(); });
  loop->Run();
}

int TestDirectSyncPing() {
  DirectTestServer ts;
  ts.Start();

  zrpc::EventLoop loop;
  zrpc::rpc::Client client(&loop, kTestIp, kTestPort);
  client.Connect(true);

  direct_example::PingRequest request;
  request.message = "world";
  const zrpc::rpc::Reply reply =
      client.Call(direct_example::kPing,
                  direct_example::EncodePingRequest(request));
  if (!reply.ok()) {
    std::cerr << "FAIL TestDirectSyncPing call: " << reply.error() << std::endl;
    ts.Stop();
    return 1;
  }

  direct_example::PingResponse response;
  if (!direct_example::DecodePingResponse(reply.body(), &response) ||
      response.message != "pong:world") {
    std::cerr << "FAIL TestDirectSyncPing response" << std::endl;
    ts.Stop();
    return 1;
  }

  client.Shutdown();
  DrainLoop(&loop);
  ts.Stop();
  std::cout << "PASS TestDirectSyncPing" << std::endl;
  return 0;
}

int TestDirectAsyncPing() {
  DirectTestServer ts;
  ts.Start();

  zrpc::EventLoop loop;
  zrpc::rpc::Client client(&loop, kTestIp, kTestPort);
  client.Connect(true);

  std::atomic<bool> done{false};
  direct_example::PingRequest request;
  request.message = "async";

  client.AsyncCall(
      direct_example::kPing, direct_example::EncodePingRequest(request),
      [&](const zrpc::rpc::Reply& reply) {
        direct_example::PingResponse response;
        if (reply.ok() &&
            direct_example::DecodePingResponse(reply.body(), &response) &&
            response.message == "pong:async") {
          done.store(true, std::memory_order_release);
        }
      });

  loop.RunAfter(0.001, true, [&done, &loop]() {
    if (done.load(std::memory_order_acquire)) {
      loop.Quit();
    }
  });
  loop.Run();

  client.Shutdown();

  if (!done.load(std::memory_order_acquire)) {
    std::cerr << "FAIL TestDirectAsyncPing" << std::endl;
    ts.Stop();
    return 1;
  }

  ts.Stop();
  std::cout << "PASS TestDirectAsyncPing" << std::endl;
  return 0;
}

int TestDirectLogin() {
  DirectTestServer ts;
  ts.Start();

  zrpc::EventLoop loop;
  zrpc::rpc::Client client(&loop, kTestIp, kTestPort);
  client.Connect(true);

  direct_example::LoginRequest request;
  request.username = "alice";
  request.password = "alice123";

  const zrpc::rpc::Reply reply =
      client.Call(direct_example::kLogin,
                  direct_example::EncodeLoginRequest(request));
  if (!reply.ok()) {
    std::cerr << "FAIL TestDirectLogin call: " << reply.error() << std::endl;
    ts.Stop();
    return 1;
  }

  direct_example::LoginResponse response;
  if (!direct_example::DecodeLoginResponse(reply.body(), &response) ||
      response.token != "token-alice" || response.player_id != "10001") {
    std::cerr << "FAIL TestDirectLogin response" << std::endl;
    ts.Stop();
    return 1;
  }

  client.Shutdown();
  DrainLoop(&loop);
  ts.Stop();
  std::cout << "PASS TestDirectLogin" << std::endl;
  return 0;
}

int TestDirectLoginRejected() {
  DirectTestServer ts;
  ts.Start();

  zrpc::EventLoop loop;
  zrpc::rpc::Client client(&loop, kTestIp, kTestPort);
  client.Connect(true);

  direct_example::LoginRequest request;
  request.username = "alice";
  request.password = "wrong";

  const zrpc::rpc::Reply reply =
      client.Call(direct_example::kLogin,
                  direct_example::EncodeLoginRequest(request));
  if (reply.ok()) {
    std::cerr << "FAIL TestDirectLoginRejected" << std::endl;
    ts.Stop();
    return 1;
  }

  client.Shutdown();
  DrainLoop(&loop);
  ts.Stop();
  std::cout << "PASS TestDirectLoginRejected" << std::endl;
  return 0;
}

int TestWorkerPoolBoundAndExceptionIsolation() {
  auto pool = std::make_shared<zrpc::rpc::WorkerPool>(1, 1);
  pool->Start();

  std::promise<void> entered;
  std::promise<void> release_promise;
  std::shared_future<void> release = release_promise.get_future().share();
  if (!pool->Post([&entered, release]() {
        entered.set_value();
        release.wait();
      })) {
    std::cerr << "FAIL TestWorkerPoolBoundAndExceptionIsolation start"
              << std::endl;
    return 1;
  }
  entered.get_future().wait();

  const bool first_queued = pool->Post([]() {
    throw std::runtime_error("expected worker exception");
  });
  const bool overflow_rejected = !pool->Post([]() {});
  release_promise.set_value();

  std::promise<void> survived;
  while (!pool->Post([&survived]() { survived.set_value(); })) {
    std::this_thread::yield();
  }
  if (survived.get_future().wait_for(std::chrono::seconds(2)) !=
      std::future_status::ready) {
    std::cerr << "FAIL TestWorkerPoolBoundAndExceptionIsolation recovery"
              << std::endl;
    pool->Stop();
    return 1;
  }
  pool->Stop();

  if (!first_queued || !overflow_rejected) {
    std::cerr << "FAIL TestWorkerPoolBoundAndExceptionIsolation bound"
              << std::endl;
    return 1;
  }
  std::cout << "PASS TestWorkerPoolBoundAndExceptionIsolation" << std::endl;
  return 0;
}

int TestWorkerPoolSelfStop() {
  auto pool = std::make_shared<zrpc::rpc::WorkerPool>(1, 8);
  pool->Start();
  std::promise<void> stopped;
  if (!pool->Post([pool, &stopped]() {
        pool->Stop();
        stopped.set_value();
      })) {
    std::cerr << "FAIL TestWorkerPoolSelfStop post" << std::endl;
    return 1;
  }
  if (stopped.get_future().wait_for(std::chrono::seconds(2)) !=
      std::future_status::ready) {
    std::cerr << "FAIL TestWorkerPoolSelfStop timeout" << std::endl;
    return 1;
  }
  std::cout << "PASS TestWorkerPoolSelfStop" << std::endl;
  return 0;
}

}  // namespace

int main() {
  int failed = 0;
  failed += TestDirectSyncPing();
  failed += TestDirectAsyncPing();
  failed += TestDirectLogin();
  failed += TestDirectLoginRejected();
  failed += TestWorkerPoolBoundAndExceptionIsolation();
  failed += TestWorkerPoolSelfStop();

  if (failed == 0) {
    std::cout << "ALL DIRECT RPC TESTS PASSED" << std::endl;
  }
  return failed;
}
