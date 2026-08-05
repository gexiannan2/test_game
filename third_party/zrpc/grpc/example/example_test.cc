#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <google/protobuf/stubs/callback.h>

#include "example_service.h"
#include "sudoku.pb.h"
#include "zrpc/grpc/rpc.pb.h"
#include "zrpc/base/logger.h"
#include "zrpc/grpc/rpc_client.h"
#include "zrpc/grpc/rpc_controller.h"
#include "zrpc/grpc/rpc_endpoint.h"
#include "zrpc/grpc/rpc_retry_policy.h"
#include "zrpc/grpc/rpc_server.h"

namespace {

constexpr uint16_t kTestPort = 16379;
constexpr char kTestIp[] = "127.0.0.1";

class TestServer {
 public:
  explicit TestServer(rpc_example::ServiceMode mode) : service_(mode) {}

  void Start() {
    thread_ = std::thread([this]() {
      zrpc::EventLoop loop;
      loop_ = &loop;
      server_ = std::make_unique<zrpc::RpcServer>(&loop, kTestIp, kTestPort);
      server_->RegisterService(&service_);
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
      stopped_.store(true, std::memory_order_release);
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

  rpc_example::SudokuServiceImpl* service() { return &service_; }

 private:
  rpc_example::SudokuServiceImpl service_;
  std::unique_ptr<zrpc::RpcServer> server_;
  zrpc::EventLoop* loop_ = nullptr;
  std::thread thread_;
  std::atomic<bool> ready_{false};
  std::atomic<bool> stopped_{false};
};

#define EXPECT_TRUE(cond, msg)                                   \
  do {                                                           \
    if (!(cond)) {                                               \
      std::cerr << "FAIL: " << msg << " (" << __FILE__ << ":"   \
                << __LINE__ << ")\n";                            \
      return 1;                                                  \
    }                                                            \
  } while (0)

void DrainLoop(zrpc::EventLoop* loop) {
  loop->RunAfter(0.001, false, [loop]() { loop->Quit(); });
  loop->Run();
}

class CountingClosure : public google::protobuf::Closure {
 public:
  explicit CountingClosure(int* count) : count_(count) {}

  void Run() override { ++(*count_); }

 private:
  int* count_;
};

int TestEndpointValidation() {
  zrpc::RpcEndpoint endpoint;
  EXPECT_TRUE(zrpc::ParseEndpoint("127.0.0.1:65535", &endpoint),
              "valid IPv4 endpoint should parse");
  EXPECT_TRUE(endpoint.port == 65535, "maximum port should be preserved");
  EXPECT_TRUE(zrpc::ParseEndpoint("[::1]:443", &endpoint),
              "valid bracketed IPv6 endpoint should parse");
  EXPECT_TRUE(endpoint.ip == "::1" && endpoint.ToString() == "[::1]:443",
              "IPv6 endpoint should round-trip");
  EXPECT_TRUE(!zrpc::ParseEndpoint("127.0.0.1:0", &endpoint),
              "zero port should be rejected");
  EXPECT_TRUE(!zrpc::ParseEndpoint("127.0.0.1:-1", &endpoint),
              "negative port should be rejected");
  EXPECT_TRUE(!zrpc::ParseEndpoint("127.0.0.1:65536", &endpoint),
              "overflowing port should be rejected");
  EXPECT_TRUE(!zrpc::ParseEndpoint("127.0.0.1:80tail", &endpoint),
              "trailing port characters should be rejected");
  std::cout << "PASS TestEndpointValidation\n";
  return 0;
}

int TestControllerCancellation() {
  zrpc::RpcController controller;
  int count = 0;
  CountingClosure before_cancel(&count);
  controller.NotifyOnCancel(&before_cancel);
  controller.StartCancel();
  controller.StartCancel();
  EXPECT_TRUE(count == 1, "cancel callback should run exactly once");

  CountingClosure after_cancel(&count);
  controller.NotifyOnCancel(&after_cancel);
  EXPECT_TRUE(count == 2, "late cancel callback should run immediately");
  std::cout << "PASS TestControllerCancellation\n";
  return 0;
}

int TestRetryClassification() {
  zrpc::RpcRetryPolicy policy;
  zrpc::RpcController controller;
  controller.SetFailed("missing service");
  controller.SetErrorCode(static_cast<int>(NO_SERVICE));
  EXPECT_TRUE(!policy.ShouldRetry(&controller, 0),
              "non-transient server errors should not retry");
  controller.SetErrorCode(static_cast<int>(TIMEOUT));
  EXPECT_TRUE(policy.ShouldRetry(&controller, 0),
              "timeouts should retry by default");
  std::cout << "PASS TestRetryClassification\n";
  return 0;
}

int TestSyncSuccess() {
  TestServer ts(rpc_example::ServiceMode::kNormal);
  ts.Start();

  zrpc::EventLoop loop;
  zrpc::RpcClient client(&loop, kTestIp, kTestPort);
  client.EnableRetry();
  client.Connect(true);

  sudoku::SudokuRequest req;
  req.set_checkerboard("001010");
  sudoku::SudokuResponse resp;
  zrpc::RpcController ctrl;
  sudoku::SudokuService::Stub stub(client.channel());
  stub.Solve(&ctrl, &req, &resp, nullptr);

  EXPECT_TRUE(!ctrl.Failed(), "sync rpc should succeed");
  EXPECT_TRUE(resp.solved(), "response should be solved");
  EXPECT_TRUE(resp.checkerboard() == "123456789", "unexpected board");

  client.Shutdown();
  DrainLoop(&loop);
  ts.Stop();
  std::cout << "PASS TestSyncSuccess\n";
  return 0;
}

int TestAsyncServiceRequestLifetime() {
  TestServer ts(rpc_example::ServiceMode::kAsync);
  ts.Start();

  zrpc::EventLoop loop;
  zrpc::RpcClient client(&loop, kTestIp, kTestPort);
  client.Connect(true);

  sudoku::SudokuRequest req;
  req.set_checkerboard("async-request-data");
  sudoku::SudokuResponse resp;
  zrpc::RpcController ctrl;
  ctrl.SetTimeout(1.0);
  sudoku::SudokuService::Stub stub(client.channel());
  stub.Solve(&ctrl, &req, &resp, nullptr);

  EXPECT_TRUE(!ctrl.Failed(), "async service should succeed");
  EXPECT_TRUE(resp.checkerboard() == req.checkerboard(),
              "async service request should remain alive");

  client.Shutdown();
  DrainLoop(&loop);
  ts.Stop();
  std::cout << "PASS TestAsyncServiceRequestLifetime\n";
  return 0;
}

int TestNotConnected() {
  zrpc::EventLoop loop;
  zrpc::RpcChannel channel;
  sudoku::SudokuRequest req;
  req.set_checkerboard("001010");
  sudoku::SudokuResponse resp;
  zrpc::RpcController ctrl;
  sudoku::SudokuService::Stub stub(&channel);
  stub.Solve(&ctrl, &req, &resp, nullptr);

  EXPECT_TRUE(ctrl.Failed(), "rpc without connect should fail");
  EXPECT_TRUE(ctrl.ErrorText().find("connection") != std::string::npos ||
                  ctrl.ErrorText().find("not ready") != std::string::npos,
              "unexpected error: " + ctrl.ErrorText());
  DrainLoop(&loop);
  std::cout << "PASS TestNotConnected\n";
  return 0;
}

int TestEmptyRequestRejected() {
  TestServer ts(rpc_example::ServiceMode::kNormal);
  ts.Start();

  zrpc::EventLoop loop;
  zrpc::RpcClient client(&loop, kTestIp, kTestPort);
  client.EnableRetry();
  client.Connect(true);

  sudoku::SudokuRequest req;
  sudoku::SudokuResponse resp;
  zrpc::RpcController ctrl;
  sudoku::SudokuService::Stub stub(client.channel());
  stub.Solve(&ctrl, &req, &resp, nullptr);

  EXPECT_TRUE(ctrl.Failed(), "empty checkerboard should fail");
  client.Shutdown();
  DrainLoop(&loop);
  ts.Stop();
  std::cout << "PASS TestEmptyRequestRejected\n";
  return 0;
}

int TestServiceFailure() {
  TestServer ts(rpc_example::ServiceMode::kFail);
  ts.Start();

  zrpc::EventLoop loop;
  zrpc::RpcClient client(&loop, kTestIp, kTestPort);
  client.EnableRetry();
  client.Connect(true);

  sudoku::SudokuRequest req;
  req.set_checkerboard("001010");
  sudoku::SudokuResponse resp;
  zrpc::RpcController ctrl;
  sudoku::SudokuService::Stub stub(client.channel());
  stub.Solve(&ctrl, &req, &resp, nullptr);

  EXPECT_TRUE(ctrl.Failed(), "injected service failure should fail");
  client.Shutdown();
  DrainLoop(&loop);
  ts.Stop();
  std::cout << "PASS TestServiceFailure\n";
  return 0;
}

int TestTimeout() {
  TestServer ts(rpc_example::ServiceMode::kSlow);
  ts.Start();

  zrpc::EventLoop loop;
  zrpc::RpcClientOptions opts;
  opts.retry.max_retries = 0;
  zrpc::RpcClient client(&loop, kTestIp, kTestPort, opts);
  client.EnableRetry();
  client.Connect(true);

  sudoku::SudokuRequest req;
  req.set_checkerboard("001010");
  sudoku::SudokuResponse resp;
  zrpc::RpcController ctrl;
  ctrl.SetTimeout(0.05);
  sudoku::SudokuService::Stub stub(client.channel());
  stub.Solve(&ctrl, &req, &resp, nullptr);

  EXPECT_TRUE(ctrl.Failed(), "slow server should timeout");
  EXPECT_TRUE(ctrl.ErrorCode() == static_cast<int>(TIMEOUT) ||
                  ctrl.ErrorText().find("timeout") != std::string::npos,
              "expected timeout error");
  client.Shutdown();
  DrainLoop(&loop);
  ts.Stop();
  std::cout << "PASS TestTimeout\n";
  return 0;
}

struct BoundedAsyncState {
  zrpc::EventLoop* loop = nullptr;
  sudoku::SudokuService::Stub* stub = nullptr;
  std::atomic<int>* pending = nullptr;
  std::atomic<int>* completed = nullptr;
  int total = 0;
  int inflight = 0;
  std::function<void()>* issue = nullptr;
};

struct BoundedAsyncCall {
  BoundedAsyncState* state = nullptr;
  sudoku::SudokuRequest* request = nullptr;
  sudoku::SudokuResponse* response = nullptr;
};

void OnBoundedAsyncDone(BoundedAsyncCall* call) {
  std::unique_ptr<BoundedAsyncCall> guard(call);
  delete call->request;
  delete call->response;
  BoundedAsyncState* s = call->state;
  s->pending->fetch_sub(1);
  const int done = s->completed->fetch_add(1) + 1;
  if (done >= s->total) {
    s->loop->Quit();
    return;
  }
  (*s->issue)();
}

int TestBoundedAsyncAndShutdown() {
  TestServer ts(rpc_example::ServiceMode::kNormal);
  ts.Start();

  zrpc::EventLoop loop;
  zrpc::RpcClientOptions opts;
  opts.pool_size = 2;
  opts.retry.max_retries = 1;
  zrpc::RpcClient client(&loop, kTestIp, kTestPort, opts);
  client.EnableRetry();
  client.Connect(true);

  constexpr int kTotal = 200;
  constexpr int kInflight = 16;
  std::atomic<int> pending{0};
  std::atomic<int> completed{0};

  std::function<void()> issue;
  sudoku::SudokuService::Stub stub(client.channel());
  BoundedAsyncState state{&loop, &stub, &pending, &completed, kTotal, kInflight,
                          &issue};

  issue = [&state]() {
    while (state.pending->load() < state.inflight &&
           state.completed->load() + state.pending->load() < state.total) {
      state.pending->fetch_add(1);
      auto* call = new BoundedAsyncCall;
      call->state = &state;
      call->request = new sudoku::SudokuRequest;
      call->request->set_checkerboard("001010");
      call->response = new sudoku::SudokuResponse;
      state.stub->Solve(nullptr, call->request, call->response,
                        google::protobuf::NewCallback(&OnBoundedAsyncDone,
                                                      call));
    }
  };
  state.issue = &issue;

  loop.RunInLoop([&issue]() { issue(); });
  loop.Run();

  EXPECT_TRUE(completed.load() == kTotal,
              "all async requests should complete before shutdown");
  client.Shutdown();
  DrainLoop(&loop);
  ts.Stop();
  std::cout << "PASS TestBoundedAsyncAndShutdown metrics="
            << client.MetricsString() << "\n";
  return 0;
}

int TestServerPrepareShutdown() {
  TestServer ts(rpc_example::ServiceMode::kNormal);
  ts.Start();

  zrpc::EventLoop loop;
  zrpc::RpcClient client(&loop, kTestIp, kTestPort);
  client.EnableRetry();
  client.Connect(true);

  sudoku::SudokuRequest req;
  req.set_checkerboard("001010");
  sudoku::SudokuResponse resp;
  zrpc::RpcController ctrl;
  sudoku::SudokuService::Stub stub(client.channel());
  stub.Solve(&ctrl, &req, &resp, nullptr);
  EXPECT_TRUE(!ctrl.Failed(), "baseline rpc should succeed");

  client.Shutdown();
  DrainLoop(&loop);
  ts.Stop();
  std::cout << "PASS TestServerPrepareShutdown\n";
  return 0;
}

#undef EXPECT_TRUE

}  // namespace

int main() {
  int failed = 0;
  failed += TestEndpointValidation();
  failed += TestControllerCancellation();
  failed += TestRetryClassification();
  failed += TestNotConnected();
  failed += TestSyncSuccess();
  failed += TestAsyncServiceRequestLifetime();
  failed += TestEmptyRequestRejected();
  failed += TestServiceFailure();
  failed += TestTimeout();
  failed += TestBoundedAsyncAndShutdown();
  failed += TestServerPrepareShutdown();

  google::protobuf::ShutdownProtobufLibrary();
  if (failed == 0) {
    std::cout << "ALL TESTS PASSED\n";
  } else {
    std::cerr << failed << " test(s) failed\n";
  }
  return failed;
}
