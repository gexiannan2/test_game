#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "zrpc/net/channel.h"
#include "zrpc/net/socket.h"

#ifdef __APPLE__
#include "zrpc/net/poll.h"
#endif

#ifdef __linux__
#include <sys/eventfd.h>

#include "zrpc/net/epoll.h"
#endif

#ifdef _WIN32
#include "zrpc/net/iocp.h"
#endif

#include "zrpc/base/group.h"
#include "zrpc/base/timer_queue.h"
#include "zrpc/net/callback.h"
namespace zrpc {

typedef std::function<void()> Functor;
typedef std::function<void(int, std::string*)> RpcRespFunctor;
typedef std::function<void(Context&)> RpcReqFunctor;

class EventLoop : public std::enable_shared_from_this<EventLoop> {
 public:
  EventLoop();

  ~EventLoop();

  void Quit();
  void QuitInLoop();
  void Run();
  // Process one I/O poll cycle; used by sync RPC on the loop thread.
  void PollOnce(int timeout_ms = 100);
  void HandleRead();

  void OnRpcReply(Context ctx, int code, std::string* reply);
  // 5000000 us = 5 seconds
  // optional last parameter `caller_loop` allows the callee to post the reply
  // back to the caller's EventLoop so the response callback runs on the
  // caller's thread. If nullptr (default), behavior is unchanged: callback
  // runs on the target loop.
  void RunRpcInLoop(uint64_t expire_us, int64_t pid, Values& values,
                    RpcReqFunctor&& req_cb, RpcRespFunctor&& resp_cb,
                    EventLoop* caller_loop = nullptr);
  void QueueRpcInLoop(uint64_t expire_us, int64_t pid, Values& values,
                      RpcReqFunctor&& req_cb, RpcRespFunctor&& resp_cb,
                      EventLoop* caller_loop = nullptr);

  void RunInLoop(Functor&& cb);
  void QueueInLoop(Functor&& cb);

  void Wakeup();
  bool UpdateChannel(Channel* channel);
  bool RemoveChannel(Channel* channel);
  bool HasChannel(Channel* channel);
#ifdef _WIN32
  void ReleaseSocketContext(SocketHandle fd);
#endif
  void CancelAfter(const std::shared_ptr<Timer>& timer);

  void AssertInLoopThread();

  std::shared_ptr<Timer> RunAfter(double when, bool repeat, TimerCallback&& cb);
  std::shared_ptr<Timer> RunAt(TimeStamp&& stamp, double when, bool repeat,
                               TimerCallback&& cb);

  void HandlerTimerQueue();
  TimerQueue* GetTimerQueue() { return timer_queue_.get(); }
  bool IsInLoopThread() const;
  bool GetEventHandling() const;
  std::thread::id GetThreadId() const;
  int64_t GetTimeOut() const;

  struct Metrics {
    uint64_t wakeup_count{0};
    uint64_t run_inloop_count{0};
    uint64_t queue_inloop_count{0};
    uint64_t queue_rpc_inloop_count{0};
    size_t max_pending_functors{0};
    size_t max_rpc_pending{0};
    size_t max_rpc_callbacks{0};
    size_t max_rpc_heap{0};
  };

  Metrics GetMetrics() const;

  // Set the engine used by this EventLoop to handle RPC calls.
  void SetEngine(std::shared_ptr<Engine> eng) { engine_ = std::move(eng); }

 private:
  EventLoop(const EventLoop&);

  void operator=(const EventLoop&);

  void AbortNotInLoopThread();
  void DoPendingFunctors();

  void CheckRpcTimeouts();

  struct CallbackTargetState {
    std::mutex mutex;
    EventLoop* loop{nullptr};
  };
  std::shared_ptr<CallbackTargetState> callback_target_state_;
  std::thread::id thread_id_;
  mutable std::mutex mutex_;
#ifdef __APPLE__
  std::shared_ptr<Poll> epoller_;
  int32_t wakeup_fd_[2]{-1, -1};
  int32_t op_;
#endif

#ifdef __linux__
  std::shared_ptr<Epoll> epoller_;
  int32_t wakeup_fd_;
#endif

#ifdef _WIN32
  std::shared_ptr<Iocp> epoller_;
  SocketHandle wakeup_fd_[2]{kInvalidSocket, kInvalidSocket};
  int32_t op_;
#endif

  std::atomic<int64_t> seq_;

  std::shared_ptr<TimerQueue> timer_queue_;
  std::shared_ptr<Channel> wakeup_channel_;
  std::shared_ptr<Engine> engine_;

  typedef std::vector<Channel*> ChannelList;
  ChannelList active_channels_;
  Channel* current_active_channel_;

  std::atomic<bool> running_;
  bool event_handling_;
  std::atomic<bool> calling_pending_functors_{false};

  std::shared_ptr<Timer> rpc_timeout_timer_;
  std::vector<Functor> functors_;
  std::vector<Functor> pending_functors_;

  std::vector<Context> rpc_functors_;
  std::vector<Context> rpc_pending_functors_;

  std::vector<Context> rpc_reply_functors_;
  std::vector<Context> rpc_pending_reply_functors_;

  std::unordered_map<int64_t, RpcRespFunctor> rpc_callback_functors_;
  std::set<TimeoutItem, TimeoutItemCmp> rpc_heap_timers_;
  std::unordered_map<int64_t, std::set<TimeoutItem, TimeoutItemCmp>::iterator> rpc_timer_map_;

  // runtime metrics
  std::atomic<uint64_t> wakeup_count_{0};
  std::atomic<uint64_t> runin_count_{0};
  std::atomic<uint64_t> queuein_count_{0};
  std::atomic<uint64_t> queue_rpc_count_{0};
  std::atomic<size_t> max_pending_functors_{0};
  std::atomic<size_t> max_rpc_pending_{0};
  std::atomic<size_t> max_rpc_callbacks_{0};
  std::atomic<size_t> max_rpc_heap_{0};
};
}  // namespace zrpc
