#include <iostream>
#include <thread>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <string>
#include <cassert>
#include <atomic>
#include <vector>
#include <chrono>

#include "zrpc/net/event_loop.h"
#include "zrpc/base/group.h"

using namespace zrpc;

std::mutex demo_mutex;
std::condition_variable demo_cond;
bool demo_ready = false;

class Node {
 public:
  void StartLoop() {
    EventLoop loop_;
    {
      std::unique_lock<std::mutex> lk(demo_mutex);
      loop = &loop_;
      demo_ready = true;
      demo_cond.notify_one();
    }

    loop_.RunAfter(0.5, false, std::bind(&Node::Test, this));
    loop_.Run();
  }

  void Test() { std::cout << "Node: Test EventLoop on thread " << std::this_thread::get_id() << std::endl; }

  // Immediate reply
  void DoRpcImmediate(Context& ctx) {
    auto seq_opt = ctx.Get<int64_t>("seq");
    std::cout << "Node.DoRpcImmediate seq=" << (seq_opt ? std::to_string(*seq_opt) : std::string("?"))
              << " thread=" << std::this_thread::get_id() << std::endl;
    std::string reply = "immediate_response";
    if (loop) {
      loop->OnRpcReply(ctx, 0, &reply);
    }
  }

  // Delayed reply (simulate async work)
  void DoRpcDelayed(Context& ctx) {
    auto seq_opt = ctx.Get<int64_t>("seq");
    std::cout << "Node.DoRpcDelayed seq=" << (seq_opt ? std::to_string(*seq_opt) : std::string("?"))
              << " thread=" << std::this_thread::get_id() << std::endl;
    Context ctx_copy = ctx;
    // schedule reply after 100ms
    if (loop) {
      loop->RunAfter(0.1, false, [this, ctx_copy]() mutable {
        std::string reply = "delayed_response";
        if (loop) {
          loop->OnRpcReply(ctx_copy, 0, &reply);
        }
      });
    }
  }

  // No reply: test timeout
  void DoRpcNoReply(Context& ctx) {
    auto seq_opt = ctx.Get<int64_t>("seq");
    std::cout << "Node.DoRpcNoReply seq=" << (seq_opt ? std::to_string(*seq_opt) : std::string("?"))
              << " thread=" << std::this_thread::get_id() << " (will not reply)" << std::endl;
    // intentionally do nothing
  }

  EventLoop* loop{nullptr};
};

int main() {
#ifdef _WIN32
  WSADATA wsaData;
  int32_t iRet = WSAStartup(MAKEWORD(2, 2), &wsaData);
  if (iRet != 0) {
    std::cerr << "WSAStartup failed: " << iRet << std::endl;
    return 1;
  }
#endif

  Node node;
  std::unique_ptr<std::thread> bg_thread_;
  bg_thread_.reset(new std::thread(std::bind(&Node::StartLoop, &node)));

  {
    std::unique_lock<std::mutex> lk(demo_mutex);
    demo_cond.wait(lk, []() { return demo_ready; });
  }

  EventLoop loop;
  EventLoop* main_loop = &loop;
  EventLoop* node_loop = node.loop;

  if (!node_loop) {
    std::cerr << "node loop is null, abort" << std::endl;
    return 1;
  }

  // Inject Engine instances to avoid nullptr dereference inside RunRpcInLoop
  main_loop->SetEngine(std::make_shared<Engine>());
  node_loop->SetEngine(std::make_shared<Engine>());

  const int kRequests = 6;
  std::atomic<int> pending{kRequests};

  // Prepare a batch of RPCs with different behaviors
  loop.RunAfter(0.2, false, [node_loop, main_loop, &node, &pending, kRequests]() mutable {
    for (int i = 0; i < kRequests; ++i) {
      Values vals;
      vals["msg"] = std::string("hello_") + std::to_string(i);

      RpcReqFunctor req_cb;
      if (i % 3 == 0) {
        req_cb = std::bind(&Node::DoRpcImmediate, &node, std::placeholders::_1);
      } else if (i % 3 == 1) {
        req_cb = std::bind(&Node::DoRpcDelayed, &node, std::placeholders::_1);
      } else {
        req_cb = std::bind(&Node::DoRpcNoReply, &node, std::placeholders::_1);
      }

      RpcRespFunctor resp_cb = [main_loop, node_loop, &pending, i](int code, std::string* reply) {
        std::cout << "Resp callback for i=" << i << " running on thread " << std::this_thread::get_id()
                  << ", code=" << code << ", reply=" << (reply ? *reply : std::string("(null)")) << std::endl;

        int left = --pending;
        if (left <= 0) {
          std::cout << "All responses processed, shutting down loops" << std::endl;
          if (main_loop) main_loop->RunInLoop(std::bind(&EventLoop::Quit, main_loop));
          if (node_loop) node_loop->RunInLoop(std::bind(&EventLoop::Quit, node_loop));
        }
      };

      // expire after 5s to avoid race with timeout checker during tests
      node_loop->RunRpcInLoop(5000000 /*5s*/, 1 /*pid*/, vals, std::move(req_cb), std::move(resp_cb), main_loop);
    }
  });

  loop.Run();

  if (bg_thread_ && bg_thread_->joinable()) bg_thread_->join();

  std::cout << "Demo finished." << std::endl;
  return 0;
}
