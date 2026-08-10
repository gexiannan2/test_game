#include <atomic>
#include <charconv>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

#include "example_service.h"
#include "sudoku.pb.h"
#include "zrpc/base/logger.h"
#include "zrpc/grpc/rpc_client.h"
#include "zrpc/grpc/rpc_closure.h"

namespace {

constexpr uint16_t kDefaultPort = 6379;
constexpr int kDefaultRequests = 1000;
constexpr int kDefaultInflight = 32;

struct ClientConfig {
  uint16_t port = kDefaultPort;
  int requests = kDefaultRequests;
  int inflight = kDefaultInflight;
};

int ParsePositiveInt(std::string_view value, int fallback, int maximum) {
  int parsed_value = 0;
  const auto parsed = std::from_chars(value.data(),
                                      value.data() + value.size(),
                                      parsed_value);
  if (parsed.ec != std::errc() ||
      parsed.ptr != value.data() + value.size() || parsed_value < 1 ||
      parsed_value > maximum) {
    return fallback;
  }
  return parsed_value;
}

ClientConfig ParseArgs(int argc, char* argv[]) {
  ClientConfig cfg;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.rfind("--port=", 0) == 0) {
      cfg.port = static_cast<uint16_t>(ParsePositiveInt(
          std::string_view(arg).substr(7), kDefaultPort,
          std::numeric_limits<uint16_t>::max()));
    } else if (arg.rfind("--requests=", 0) == 0) {
      cfg.requests = ParsePositiveInt(std::string_view(arg).substr(11),
                                      kDefaultRequests, 100000000);
    } else if (arg.rfind("--inflight=", 0) == 0) {
      cfg.inflight = ParsePositiveInt(std::string_view(arg).substr(11),
                                      kDefaultInflight, 1000000);
    }
  }
  return cfg;
}

class BenchmarkClient : public std::enable_shared_from_this<BenchmarkClient> {
 public:
  BenchmarkClient(zrpc::EventLoop* loop, uint16_t port, int requests,
                  int inflight)
      : loop_(loop),
        requests_(requests),
        inflight_limit_(inflight),
        rpc_client_(loop, "127.0.0.1", port),
        stub_(rpc_client_.channel()) {
    rpc_client_.EnableRetry();
  }

  void Start() {
    std::shared_ptr<BenchmarkClient> self = shared_from_this();
    rpc_client_.SetConnectionCallback(
        [self = std::move(self)](
            const std::shared_ptr<zrpc::TcpConnection>& conn) {
          self->OnConnection(conn);
        });
    rpc_client_.Connect();
  }

 private:
  void OnConnection(const std::shared_ptr<zrpc::TcpConnection>& conn) {
    if (!conn->Connected()) {
      if (!started_.load(std::memory_order_relaxed) &&
          issued_.load(std::memory_order_relaxed) == 0) {
        LOG_WARN << "rpc client disconnected before benchmark started";
      }
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

  void OnRpcDone() {
    completed_.fetch_add(1, std::memory_order_relaxed);
    pending_.fetch_sub(1, std::memory_order_relaxed);

    if (completed_.load(std::memory_order_relaxed) >= requests_) {
      LOG_INFO << "benchmark done completed=" << completed_.load()
               << " metrics=" << rpc_client_.MetricsString();
      loop_->Quit();
      return;
    }
    IssueMore();
  }

  void AsyncSolve() {
    auto request = std::make_unique<sudoku::SudokuRequest>();
    request->set_checkerboard("001010");
    auto response = std::make_unique<sudoku::SudokuResponse>();

    sudoku::SudokuRequest* req = request.get();
    sudoku::SudokuResponse* resp = response.get();
    auto self = shared_from_this();

    stub_.Solve(
        nullptr, req, resp,
        zrpc::NewRpcCallback([self, request = std::move(request),
                              response = std::move(response)]() mutable {
          (void)request;
          (void)response;
          self->OnRpcDone();
        }));
  }

  void IssueMore() {
    while (pending_.load(std::memory_order_relaxed) < inflight_limit_ &&
           issued_.load(std::memory_order_relaxed) < requests_) {
      issued_.fetch_add(1, std::memory_order_relaxed);
      pending_.fetch_add(1, std::memory_order_relaxed);
      AsyncSolve();
    }
  }

  zrpc::EventLoop* loop_;
  int requests_;
  int inflight_limit_;
  zrpc::RpcClient rpc_client_;
  sudoku::SudokuService::Stub stub_;
  std::atomic<bool> started_{false};
  std::atomic<int> issued_{0};
  std::atomic<int> pending_{0};
  std::atomic<int> completed_{0};
};

}  // namespace

int main(int argc, char* argv[]) {
  const ClientConfig cfg = ParseArgs(argc, argv);
  zrpc::EventLoop loop;
  auto client = std::make_shared<BenchmarkClient>(&loop, cfg.port, cfg.requests,
                                                  cfg.inflight);
  client->Start();
  loop.Run();
  google::protobuf::ShutdownProtobufLibrary();
  return 0;
}
