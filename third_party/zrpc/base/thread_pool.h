#pragma once

#include "zrpc/base/thread.h"
#include "zrpc/net/callback.h"

#include <cstddef>
#include <stdexcept>

namespace zrpc {
class EventLoop;
class ThreadPool {
 public:
  typedef std::function<void(EventLoop *)> ThreadInitCallback;

  ThreadPool(EventLoop *base_loop);

  ~ThreadPool();

  void SetThreadNum(int num_threads) {
    if (started_) {
      throw std::logic_error("ThreadPool is already started");
    }
    if (num_threads < 0) {
      throw std::invalid_argument("ThreadPool size must not be negative");
    }
    num_threads_ = num_threads;
  }
  void Start(const ThreadInitCallback &cb = ThreadInitCallback());
  void Stop();

  EventLoop *GetLoop(std::thread::id thread_id);
  EventLoop *GetBaseLoop();
  EventLoop *GetNextLoop();
  EventLoop *GetLoopForHash(size_t hash_code);
  std::vector<EventLoop *> GetAllLoops();
  bool GetStarted() const { return started_; }

 private:
  ThreadPool(const ThreadPool &);

  void operator=(const ThreadPool &);

  EventLoop *base_loop_;
  bool started_;
  int32_t num_threads_;
  size_t next_;

  std::vector<std::shared_ptr<Thread>> threads_;
  std::vector<EventLoop *> loops_;
};

}  // namespace zrpc
