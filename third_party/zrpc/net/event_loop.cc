#include "zrpc/net/event_loop.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <exception>
#include <stdexcept>

#include "zrpc/base/log.h"
#include "zrpc/base/logger.h"
#include "zrpc/base/time_stamp.h"

namespace zrpc {
namespace {
inline void AtomicMax(std::atomic<size_t>& a, size_t v) {
  size_t cur = a.load(std::memory_order_relaxed);
  while (cur < v && !a.compare_exchange_weak(cur, v,
                                             std::memory_order_relaxed)) {
  }
}

template <typename Callback>
bool InvokeLoopCallback(const char* name, Callback&& callback) {
  try {
    callback();
    return true;
  } catch (const std::exception& ex) {
    LOG_WARN << name << " threw an exception: " << ex.what();
  } catch (...) {
    LOG_WARN << name << " threw an unknown exception";
  }
  return false;
}

class PendingFunctorGuard {
 public:
  explicit PendingFunctorGuard(std::atomic<bool>* calling)
      : calling_(calling) {
    calling_->store(true, std::memory_order_release);
  }

  ~PendingFunctorGuard() {
    calling_->store(false, std::memory_order_release);
  }

 private:
  std::atomic<bool>* calling_;
};

class LoopEventHandlingGuard {
 public:
  explicit LoopEventHandlingGuard(bool* handling) : handling_(handling) {
    *handling_ = true;
  }

  ~LoopEventHandlingGuard() { *handling_ = false; }

 private:
  bool* handling_;
};
}  // namespace
#ifdef __linux__
int CreateEventfd() { return ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC); }
#endif

#ifdef _WIN32
EventLoop::EventLoop()
    : callback_target_state_(std::make_shared<CallbackTargetState>()),
      thread_id_(std::this_thread::get_id()),
      epoller_(new Iocp(this)),
      op_(socket::Pipe(wakeup_fd_)),
      wakeup_channel_(new Channel(this, wakeup_fd_[0])),
      seq_(0),
      timer_queue_(new TimerQueue(this)),
      current_active_channel_(nullptr),
      running_(false),
      event_handling_(false),
      calling_pending_functors_(false) {
  callback_target_state_->loop = this;
  wakeup_channel_->SetReadCallback(std::bind(&EventLoop::HandleRead, this));
  if (op_ < 0) {
    LOG_FATAL << "EventLoop wakeup pipe creation failed";
  }
  wakeup_channel_->EnableReading();

  rpc_timeout_timer_ =
      RunAfter(1, true, std::bind(&EventLoop::CheckRpcTimeouts, this));
}
#endif

#ifdef __linux__
EventLoop::EventLoop()
    : callback_target_state_(std::make_shared<CallbackTargetState>()),
      thread_id_(std::this_thread::get_id()),
      epoller_(new Epoll(this)),
      wakeup_fd_(CreateEventfd()),
      seq_(0),
      timer_queue_(new TimerQueue(this)),
      wakeup_channel_(new Channel(this, wakeup_fd_)),
      current_active_channel_(nullptr),
      running_(false),
      event_handling_(false),
      calling_pending_functors_(false) {
  callback_target_state_->loop = this;
  wakeup_channel_->SetReadCallback(std::bind(&EventLoop::HandleRead, this));
  if (wakeup_fd_ < 0) {
    LOG_FATAL << "EventLoop eventfd creation failed";
  }
  wakeup_channel_->EnableReading();

  rpc_timeout_timer_ =
      RunAfter(1, true, std::bind(&EventLoop::CheckRpcTimeouts, this));
}
#endif

#ifdef __APPLE__
EventLoop::EventLoop()
    : callback_target_state_(std::make_shared<CallbackTargetState>()),
      thread_id_(std::this_thread::get_id()),
      epoller_(new Poll(this)),
      op_(socketpair(AF_UNIX, SOCK_STREAM, 0, wakeup_fd_)),
      wakeup_channel_(new Channel(this, wakeup_fd_[1])),
      seq_(0),
      timer_queue_(new TimerQueue(this)),
      current_active_channel_(nullptr),
      running_(false),
      event_handling_(false),
      calling_pending_functors_(false) {
  callback_target_state_->loop = this;
  wakeup_channel_->SetReadCallback(std::bind(&EventLoop::HandleRead, this));
  if (op_ < 0) {
    LOG_FATAL << "EventLoop wakeup socketpair creation failed";
  }
  for (int fd : wakeup_fd_) {
    const int flags = ::fcntl(fd, F_GETFD, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0 ||
        !socket::SetSocketNonBlock(fd)) {
      LOG_FATAL << "EventLoop wakeup socketpair setup failed";
    }
  }
  wakeup_channel_->EnableReading();

  rpc_timeout_timer_ =
      RunAfter(1, true, std::bind(&EventLoop::CheckRpcTimeouts, this));
}
#endif

void EventLoop::AbortNotInLoopThread() {
  assert(false);
  std::abort();
}

EventLoop::~EventLoop() {
  AssertInLoopThread();
  {
    std::lock_guard<std::mutex> lock(callback_target_state_->mutex);
    callback_target_state_->loop = nullptr;
  }
  if (rpc_timeout_timer_) {
    timer_queue_->CancelTimer(rpc_timeout_timer_);
    rpc_timeout_timer_.reset();
  }
  wakeup_channel_->DisableAll();
  wakeup_channel_->Remove();
#ifdef _WIN32
  ReleaseSocketContext(wakeup_fd_[0]);
#endif
#ifdef __linux__
  socket::Close(wakeup_fd_);
#else
  socket::Close(wakeup_fd_[0]);
  socket::Close(wakeup_fd_[1]);
#endif
}

void EventLoop::AssertInLoopThread() {
  if (!IsInLoopThread()) {
    AbortNotInLoopThread();
  }
}

void EventLoop::HandlerTimerQueue() { timer_queue_->HandleRead(); }

bool EventLoop::IsInLoopThread() const {
  return thread_id_ == std::this_thread::get_id();
}

bool EventLoop::GetEventHandling() const { return event_handling_; }

std::thread::id EventLoop::GetThreadId() const { return thread_id_; }

bool EventLoop::UpdateChannel(Channel* channel) {
  assert(channel->OwnerLoop() == this);
  AssertInLoopThread();
  return epoller_->UpdateChannel(channel);
}

bool EventLoop::RemoveChannel(Channel* channel) {
  assert(channel->OwnerLoop() == this);
  AssertInLoopThread();
  if (event_handling_) {
    assert(current_active_channel_ == channel ||
           std::find(active_channels_.begin(), active_channels_.end(),
                     channel) == active_channels_.end());
  }
  return epoller_->RemoveChannel(channel);
}

void EventLoop::CancelAfter(const std::shared_ptr<Timer>& timer) {
  timer_queue_->CancelTimer(timer);
}

std::shared_ptr<Timer> EventLoop::RunAfter(double when, bool repeat,
                                           TimerCallback&& cb) {
  return timer_queue_->AddTimer(when, repeat, std::move(cb));
}

std::shared_ptr<Timer> EventLoop::RunAt(TimeStamp&& stamp, double when,
                                        bool repeat, TimerCallback&& cb) {
  return timer_queue_->AddTimer(std::move(stamp), when, repeat, std::move(cb));
}

bool EventLoop::HasChannel(Channel* channel) {
  assert(channel->OwnerLoop() == this);
  AssertInLoopThread();
  return epoller_->HasChannel(channel);
}

#ifdef _WIN32
void EventLoop::ReleaseSocketContext(SocketHandle fd) {
  AssertInLoopThread();
  epoller_->ReleaseSocketContext(fd);
}
#endif

void EventLoop::HandleRead() {
  uint64_t one = 0;
  size_t total = 0;
  while (total < sizeof one) {
    ssize_t n = 0;
#ifdef __linux__
    n = socket::Read(wakeup_fd_, reinterpret_cast<char*>(&one) + total,
                     sizeof(one) - total);
#endif

#ifdef __APPLE__
    n = socket::Read(wakeup_fd_[1], reinterpret_cast<char*>(&one) + total,
                     sizeof(one) - total);
#endif

#ifdef _WIN32
    n = socket::Read(wakeup_fd_[0], reinterpret_cast<char*>(&one) + total,
                     static_cast<int32_t>(sizeof(one) - total));
#endif
    if (n < 0) {
#ifdef _WIN32
      if (WSAGetLastError() == WSAEINTR) {
        continue;
      }
#else
      if (errno == EINTR) {
        continue;
      }
#endif
      return;
    }
    if (n == 0) {
      return;
    }
    total += static_cast<size_t>(n);
  }
}

void EventLoop::Quit() {
  running_.store(false, std::memory_order_release);
  if (!IsInLoopThread()) {
    Wakeup();
  }
}

void EventLoop::QuitInLoop() { Quit(); }

void EventLoop::Wakeup() {
  wakeup_count_.fetch_add(1, std::memory_order_relaxed);
  uint64_t one = 1;
#ifdef __linux__
  ssize_t n = socket::Write(wakeup_fd_, &one, sizeof one);
  if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
    LOG_WARN << "EventLoop::Wakeup eventfd write failed";
  }
#endif

#ifdef __APPLE__
  ssize_t n = socket::Write(wakeup_fd_[0], &one, sizeof one);
  if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
    LOG_WARN << "EventLoop::Wakeup socketpair write failed";
  }
#endif

#ifdef _WIN32
  ssize_t n = socket::Write(wakeup_fd_[1], &one, sizeof(one));
  if (n != sizeof one) {
    LOG_WARN << "EventLoop::Wakeup failed, fd=" << wakeup_fd_[1]
             << " error=" << WSAGetLastError();
  }
#endif
}

void EventLoop::RunInLoop(Functor&& cb) {
  runin_count_.fetch_add(1, std::memory_order_relaxed);
  if (IsInLoopThread()) {
    cb();
  } else {
    QueueInLoop(std::move(cb));
  }
}

void EventLoop::QueueInLoop(Functor&& cb) {
  bool needWake = false;
  bool calling_pending = false;
  {
    std::unique_lock<std::mutex> lk(mutex_);
    needWake = pending_functors_.empty();
    pending_functors_.push_back(std::move(cb));
    calling_pending = calling_pending_functors_.load(std::memory_order_relaxed);
    // track queue stats
    queuein_count_.fetch_add(1, std::memory_order_relaxed);
    AtomicMax(max_pending_functors_, pending_functors_.size());
  }

  if (calling_pending || (needWake && !IsInLoopThread())) {
    Wakeup();
  }
}

void EventLoop::DoPendingFunctors() {
  PendingFunctorGuard pending_guard(&calling_pending_functors_);

  {
    std::unique_lock<std::mutex> lk(mutex_);
    functors_.swap(pending_functors_);
  }

  for (size_t i = 0; i < functors_.size(); ++i) {
    InvokeLoopCallback("EventLoop pending functor",
                       [&]() { functors_[i](); });
  }

  {
    std::unique_lock<std::mutex> lk(mutex_);
      rpc_functors_.swap(rpc_pending_functors_);
    }

    for (size_t i = 0; i < rpc_functors_.size(); ++i) {
    auto&& ctx = rpc_functors_[i];
    auto pid_opt = ctx.Get<int64_t>("pid");
    if (!pid_opt) {
      continue;
    }

    if (!engine_) {
      auto seq_opt = ctx.Get<int64_t>("seq");
      if (seq_opt) {
        RpcRespFunctor cb;
        {
          std::unique_lock<std::mutex> lk(mutex_);
          auto it = rpc_callback_functors_.find(*seq_opt);
          if (it != rpc_callback_functors_.end()) {
            cb = std::move(it->second);
            rpc_callback_functors_.erase(it);
          }
          auto timer_it = rpc_timer_map_.find(*seq_opt);
          if (timer_it != rpc_timer_map_.end()) {
            rpc_heap_timers_.erase(timer_it->second);
            rpc_timer_map_.erase(timer_it);
          }
        }
        if (cb) {
          InvokeLoopCallback("EventLoop RPC unavailable callback",
                             [&]() { cb(-1, nullptr); });
        }
      }
      continue;
    }

    auto functor_opt = ctx.Get<RpcReqFunctor>("functor");
    if (!functor_opt) {
      InvokeLoopCallback("EventLoop RPC dispatch", [&]() {
        engine_->Do(*pid_opt, std::move(ctx.ValuesRef()));
      });
      continue;
    }

    InvokeLoopCallback("EventLoop RPC call", [&]() {
      engine_->Call(*pid_opt, std::move(ctx.ValuesRef()),
                    std::move(*functor_opt));
    });
  }

  {
    std::unique_lock<std::mutex> lk(mutex_);
    rpc_reply_functors_.swap(rpc_pending_reply_functors_);
  }

  for (size_t i = 0; i < rpc_reply_functors_.size(); ++i) {
    auto&& ctx = rpc_reply_functors_[i];
    auto seq_opt = ctx.Get<int64_t>("seq");
    if (!seq_opt) {
      continue;
    }


    auto code_opt = ctx.Get<int>("code");
    if (!code_opt) {
      continue;
    }

    auto&& bytes_opt = ctx.Get<std::string>("bytes");
    if (!bytes_opt) {
      continue;
    }

    RpcRespFunctor cb;
    {
      std::unique_lock<std::mutex> lk(mutex_);
      auto it = rpc_callback_functors_.find(*seq_opt);
      if (it != rpc_callback_functors_.end()) {
        cb = std::move(it->second);
        rpc_callback_functors_.erase(it);
      }
      auto timer_it = rpc_timer_map_.find(*seq_opt);
      if (timer_it != rpc_timer_map_.end()) {
        rpc_heap_timers_.erase(timer_it->second);
        rpc_timer_map_.erase(timer_it);
      }
    }

    if (cb) {
      InvokeLoopCallback("EventLoop RPC reply callback",
                         [&]() { cb(*code_opt, &(*bytes_opt)); });
    }
  }

  rpc_reply_functors_.clear();
  functors_.clear();
  rpc_functors_.clear();
}

void EventLoop::CheckRpcTimeouts() {
  AssertInLoopThread();
  uint64_t now = NowMicros();
  while (true) {
    TimeoutItem item;
    {
      std::unique_lock<std::mutex> lk(mutex_);
      if (rpc_heap_timers_.empty()) break;
      auto it = rpc_heap_timers_.begin();
      if (it->expire_us_ > now) break;
      item = *it;
      rpc_timer_map_.erase(item.seq_);
      rpc_heap_timers_.erase(it);
    }

    RpcRespFunctor cb;
    {
      std::unique_lock<std::mutex> lk(mutex_);
      auto it = rpc_callback_functors_.find(item.seq_);
      if (it != rpc_callback_functors_.end()) {
        cb = std::move(it->second);
        rpc_callback_functors_.erase(it);
      }
    }

    if (cb) {
      InvokeLoopCallback("EventLoop RPC timeout callback",
                         [&]() { cb(-1, nullptr); });
    }
  }
}

void EventLoop::PollOnce(int timeout_ms) {
  AssertInLoopThread();
  active_channels_.clear();
  epoller_->EpollWait(&active_channels_, timeout_ms);
  {
    LoopEventHandlingGuard event_guard(&event_handling_);
    for (auto& it : active_channels_) {
      current_active_channel_ = it;
      InvokeLoopCallback("EventLoop channel callback",
                         [&]() { current_active_channel_->HandleEvent(); });
    }
  }

  current_active_channel_ = nullptr;
  DoPendingFunctors();
}

void EventLoop::Run() {
  AssertInLoopThread();
  if (running_.exchange(true, std::memory_order_acq_rel)) {
    throw std::logic_error("EventLoop::Run called while already running");
  }
  try {
    while (running_.load(std::memory_order_acquire)) {
      PollOnce(static_cast<int>(GetTimeOut()));
    }
  } catch (...) {
    running_.store(false, std::memory_order_release);
    throw;
  }
}

int64_t EventLoop::GetTimeOut() const { return timer_queue_->GetTimeout(); }

void EventLoop::QueueRpcInLoop(uint64_t expire_us, int64_t pid, Values& values,
                               RpcReqFunctor&& req_cb,
                               RpcRespFunctor&& resp_cb,
                               EventLoop* caller_loop) {
  int64_t seq = seq_.fetch_add(1) + 1;
  values["seq"] = seq;
  values["pid"] = pid;
  values["functor"] = std::move(req_cb);
  Context ctx(pid, std::move(values));

  bool needWake = false;
  bool calling_pending = false;
  uint64_t cur_expire_us = static_cast<uint64_t>(NowMicros()) + expire_us;
  {
    std::unique_lock<std::mutex> lk(mutex_);

    // prepare response callback mapping
    if (caller_loop && caller_loop != this) {
      auto sp_resp = std::make_shared<RpcRespFunctor>(std::move(resp_cb));
      std::weak_ptr<CallbackTargetState> weak_target =
          caller_loop->callback_target_state_;
      rpc_callback_functors_.emplace(
          seq, [weak_target, sp_resp](int code, std::string* reply) {
            std::shared_ptr<std::string> reply_copy;
            if (reply) {
              reply_copy = std::make_shared<std::string>(*reply);
            }
            auto target = weak_target.lock();
            if (!target) {
              return;
            }
            std::lock_guard<std::mutex> lock(target->mutex);
            if (target->loop == nullptr) {
              return;
            }
            target->loop->QueueInLoop([sp_resp, code, reply_copy]() {
              (*sp_resp)(code, reply_copy ? const_cast<std::string*>(reply_copy.get())
                                          : nullptr);
            });
          });
    } else {
      rpc_callback_functors_.emplace(seq, std::move(resp_cb));
    }

    needWake = rpc_pending_functors_.empty();
    rpc_pending_functors_.emplace_back(std::move(ctx));
    auto [timer_it, _] = rpc_heap_timers_.insert(TimeoutItem{cur_expire_us, seq, pid});
    rpc_timer_map_.emplace(seq, timer_it);
    // metrics
    queue_rpc_count_.fetch_add(1, std::memory_order_relaxed);
    AtomicMax(max_rpc_pending_, rpc_pending_functors_.size());
    AtomicMax(max_rpc_callbacks_, rpc_callback_functors_.size());
    AtomicMax(max_rpc_heap_, rpc_heap_timers_.size());
    calling_pending = calling_pending_functors_.load(std::memory_order_relaxed);
  }

  if (calling_pending || (needWake && !IsInLoopThread())) {
    Wakeup();
  }
}

void EventLoop::OnRpcReply(Context ctx, int code, std::string* reply) {
  ctx.Set("code", code);
  ctx.Set("bytes", reply ? std::move(*reply) : std::string());
  {
    std::unique_lock<std::mutex> lk(mutex_);
    rpc_pending_reply_functors_.emplace_back(std::move(ctx));
  }

  if (!IsInLoopThread() || calling_pending_functors_) {
    Wakeup();
  }
}

void EventLoop::RunRpcInLoop(uint64_t expire_us, int64_t pid, Values& values,
                             RpcReqFunctor&& req_cb, RpcRespFunctor&& resp_cb,
                             EventLoop* caller_loop) {
  QueueRpcInLoop(expire_us, pid, values, std::move(req_cb),
                 std::move(resp_cb), caller_loop);
}


#if 0

// Simple demo (kept disabled). 说明：该示例演示如何在两个 EventLoop 线程之间
// 发起异步 RPC。示例已修正为正确的 API 名称与回调签名。

std::mutex demo_mutex;
std::condition_variable demo_cond;

class Node {
 public:
  void StartLoop() {
    EventLoop loop_;
    // 将 loop 指针设置好后通知主线程
    {
      std::unique_lock<std::mutex> lk(demo_mutex);
      loop = &loop_;
      demo_cond.notify_one();
    }

    loop_.RunAfter(1.0, false, std::bind(&Node::Test, this));
    loop_.Run();
  }

  void Test() { std::cout << "Test Node EventLoop" << std::endl; }

  // Rpc 请求处理器：接收 Context 并通过 loop->OnRpcReply 发送回复
  void DoRpc(Context& ctx) {
    auto seq_opt = ctx.Get<int64_t>("seq");
    if (seq_opt) {
      std::cout << "Node DoRpc, seq=" << *seq_opt << std::endl;
    }

    std::string reply = "response_str_";
    if (loop) {
      loop->OnRpcReply(ctx, 0, &reply);
    }
  }

  EventLoop* loop{nullptr};
};

// 响应回调，签名匹配 RpcRespFunctor
void OnRpcReply(int code, std::string* reply) {
  std::cout << "response code: " << code
            << ", reply: " << (reply ? *reply : std::string("(null)"))
            << std::endl;
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
  WSADATA wsaData;
  int32_t iRet = WSAStartup(MAKEWORD(2, 2), &wsaData);
  assert(iRet == 0);
#endif

  // 利用 eventloop 做线程之间 RPC 调用测试
  Node node;
  std::unique_ptr<std::thread> bg_thread_;
  bg_thread_.reset(new std::thread(std::bind(&Node::StartLoop, &node)));
  {
    std::unique_lock<std::mutex> lk(demo_mutex);
    demo_cond.wait(lk);
  }

  EventLoop loop;
  // 准备请求数据
  Values vals;
  vals["msg"] = std::string("hello");

  // 在主 loop 中安排一次任务，调用后台 loop 的 RunRpcInLoop 发起请求
  loop.RunAfter(1.0, false, [&node, vals]() mutable {
    // 注意：RunRpcInLoop 的第一个参数是超时时间（微秒）
    if (node.loop) {
      node.loop->RunRpcInLoop(5000000 /*5s*/, 1 /*pid*/, vals,
                              std::bind(&Node::DoRpc, &node, std::placeholders::_1),
                              std::bind(&OnRpcReply, std::placeholders::_1,
                                        std::placeholders::_2));
    }
  });

  loop.Run();

  return 0;
}

#endif

EventLoop::Metrics EventLoop::GetMetrics() const {
  Metrics m;
  m.wakeup_count = wakeup_count_.load(std::memory_order_relaxed);
  m.run_inloop_count = runin_count_.load(std::memory_order_relaxed);
  m.queue_inloop_count = queuein_count_.load(std::memory_order_relaxed);
  m.queue_rpc_inloop_count = queue_rpc_count_.load(std::memory_order_relaxed);
  m.max_pending_functors = max_pending_functors_.load(std::memory_order_relaxed);
  m.max_rpc_pending = max_rpc_pending_.load(std::memory_order_relaxed);
  m.max_rpc_callbacks = max_rpc_callbacks_.load(std::memory_order_relaxed);
  m.max_rpc_heap = max_rpc_heap_.load(std::memory_order_relaxed);
  return m;
}

}  // namespace zrpc
