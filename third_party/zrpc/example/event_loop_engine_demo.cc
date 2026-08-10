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

  void DoRpcImmediate(Context& ctx) {
    auto seq_opt = ctx.Get<int64_t>("seq");
    auto msg_opt = ctx.Get<std::string>("msg");
    std::cout << "Node.DoRpcImmediate seq=" << (seq_opt ? std::to_string(*seq_opt) : std::string("?"))
              << " msg=" << (msg_opt ? *msg_opt : std::string("(no-msg)"))
              << " thread=" << std::this_thread::get_id() << std::endl;
    std::string reply = "immediate_response";
    if (loop) {
      loop->OnRpcReply(ctx, 0, &reply);
    }
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

  // Create engine for the node loop and register middleware for pid=1
  auto node_eng = std::make_shared<Engine>();

  // Middleware 1: logging
  HandleAction mw_log = [](Context& ctx) {
    auto pid = ctx.Id();
    auto msg_opt = ctx.Get<std::string>("msg");
    std::cout << "Engine.MW log pid=" << pid << " msg=" << (msg_opt ? *msg_opt : std::string("(no-msg)"))
              << " thread=" << std::this_thread::get_id() << std::endl;
    ctx.Next();
  };

  // Middleware 2: modify the message
  HandleAction mw_modify = [](Context& ctx) {
    auto msg_opt = ctx.Get<std::string>("msg");
    std::string new_msg = "[modified] ";
    if (msg_opt) new_msg += *msg_opt;
    ctx.Set("msg", new_msg);
    ctx.Next();
  };

  // Middleware 3: optional blocker (commented out in normal run)
  HandleAction mw_block = [](Context& ctx) {
    auto msg_opt = ctx.Get<std::string>("msg");
    if (msg_opt && msg_opt->find("drop") != std::string::npos) {
      std::cout << "Engine.MW block: dropping message=" << *msg_opt << std::endl;
      ctx.Abort(); // do not call next -> handler won't run
    } else {
      ctx.Next();
    }
  };

  // Register chain for pid=1: mw_log -> mw_modify -> placeholder for actual handler
  node_eng->RegisterInternal(1, std::vector<HandleAction>{mw_log, mw_modify, HandleAction()});

  // Inject engine into node loop
  node_loop->SetEngine(node_eng);

  // main loop engine to avoid nullptr (not strictly needed for replies)
  main_loop->SetEngine(std::make_shared<Engine>());

  std::atomic<int> pending{3};

  loop.RunAfter(0.2, false, [node_loop, main_loop, &node, &pending]() mutable {
    for (int i = 0; i < 3; ++i) {
      Values vals;
      vals["msg"] = std::string((i == 2) ? "drop_me" : std::string("hello_") + std::to_string(i));

      RpcReqFunctor req_cb = std::bind(&Node::DoRpcImmediate, &node, std::placeholders::_1);

      RpcRespFunctor resp_cb = [main_loop, node_loop, &pending, i](int code, std::string* reply) {
        std::cout << "Resp callback for i=" << i << " running on thread " << std::this_thread::get_id()
                  << ", code=" << code << ", reply=" << (reply ? *reply : std::string("(null)")) << std::endl;
        int left = --pending;
        if (left <= 0) {
          if (main_loop) main_loop->RunInLoop(std::bind(&EventLoop::Quit, main_loop));
          if (node_loop) node_loop->RunInLoop(std::bind(&EventLoop::Quit, node_loop));
        }
      };

      // expire after 5s
      node_loop->RunRpcInLoop(5000000 /*5s*/, 1 /*pid*/, vals, std::move(req_cb), std::move(resp_cb), main_loop);
    }
  });

  loop.Run();

  if (bg_thread_ && bg_thread_->joinable()) bg_thread_->join();

  std::cout << "Engine demo finished." << std::endl;
  return 0;
}
