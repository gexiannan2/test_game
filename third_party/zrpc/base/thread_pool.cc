#include "zrpc/base/thread_pool.h"

#include "zrpc/base/thread.h"
#include "zrpc/net/event_loop.h"
namespace zrpc {

ThreadPool::ThreadPool(EventLoop *base_loop_)
    : base_loop_(base_loop_), started_(false), num_threads_(0), next_(0) {
  if (base_loop_ == nullptr) {
    throw std::invalid_argument("ThreadPool base loop must not be null");
  }
}

ThreadPool::~ThreadPool() { Stop(); }

void ThreadPool::Stop() {
  if (!started_) {
    return;
  }
  for (auto &t : threads_) {
    t->StopLoop();
  }
  threads_.clear();
  loops_.clear();
  started_ = false;
  next_ = 0;
}

void ThreadPool::Start(const ThreadInitCallback &cb) {
  if (started_) {
    throw std::logic_error("ThreadPool is already started");
  }
  base_loop_->AssertInLoopThread();

  started_ = true;

  try {
    for (int i = 0; i < num_threads_; i++) {
      auto thread = std::make_shared<Thread>(cb);
      threads_.push_back(thread);
      loops_.push_back(thread->StartLoop());
    }
  } catch (...) {
    Stop();
    throw;
  }

  if (num_threads_ == 0 && cb) {
    cb(base_loop_);
  }
}

EventLoop *ThreadPool::GetNextLoop() {
  if (!started_) {
    throw std::logic_error("ThreadPool is not started");
  }
  EventLoop *loop = base_loop_;

  if (!loops_.empty()) {
    loop = loops_[next_];
    ++next_;
    if (next_ >= loops_.size()) {
      next_ = 0;
    }
  }
  return loop;
}


EventLoop *ThreadPool::GetLoop(std::thread::id thread_id) {
  if (!started_) {
    throw std::logic_error("ThreadPool is not started");
  }
  if (thread_id == base_loop_->GetThreadId()) {
    return base_loop_;
  }
  for (auto *loop : loops_) {
    if (thread_id == loop->GetThreadId()) {
      return loop;
    }
  }
  return nullptr;
}

EventLoop *ThreadPool::GetBaseLoop() { return base_loop_; }

EventLoop *ThreadPool::GetLoopForHash(size_t hash_code) {
  base_loop_->AssertInLoopThread();
  EventLoop *loop = base_loop_;

  if (!loops_.empty()) {
    loop = loops_[hash_code % loops_.size()];
  }
  return loop;
}

std::vector<EventLoop *> ThreadPool::GetAllLoops() {
  if (!started_) {
    throw std::logic_error("ThreadPool is not started");
  }
  if (loops_.empty()) {
    return std::vector<EventLoop *>(1, base_loop_);
  } else {
    return loops_;
  }
}
}  // namespace zrpc
