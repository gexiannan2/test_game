#include "zrpc/base/thread.h"

#include <condition_variable>
#include <exception>
#include <mutex>
#include <stdexcept>

#include "zrpc/net/event_loop.h"

namespace zrpc {
struct Thread::SharedState {
  enum class Status {
    kNotStarted,
    kStarting,
    kRunning,
    kStopped,
    kFailed,
  };

  explicit SharedState(ThreadInitCallback init_callback)
      : callback(std::move(init_callback)) {}

  std::mutex mutex;
  std::condition_variable condition;
  EventLoop *loop{nullptr};
  ThreadInitCallback callback;
  std::exception_ptr startup_exception;
  Status status{Status::kNotStarted};
  bool stop_requested{false};
};

Thread::Thread(const ThreadInitCallback &cb)
    : state_(std::make_shared<SharedState>(cb)) {}

Thread::~Thread() { StopLoop(); }

void Thread::StopLoop() {
  std::unique_ptr<std::thread> thread;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->stop_requested = true;
    if (state_->loop != nullptr) {
      state_->loop->Quit();
    }
    if (bg_thread_ != nullptr && bg_thread_->joinable()) {
      thread = std::move(bg_thread_);
    }
  }
  if (thread == nullptr) {
    return;
  }
  if (thread->get_id() == std::this_thread::get_id()) {
    thread->detach();
  } else {
    thread->join();
  }
}

EventLoop *Thread::StartLoop() {
  std::unique_lock<std::mutex> lock(state_->mutex);
  if (state_->status != SharedState::Status::kNotStarted) {
    throw std::logic_error("Thread::StartLoop called more than once");
  }
  state_->status = SharedState::Status::kStarting;
  try {
    bg_thread_ =
        std::make_unique<std::thread>(&Thread::ThreadFunc, state_);
  } catch (...) {
    state_->status = SharedState::Status::kNotStarted;
    throw;
  }
  state_->condition.wait(lock, [this]() {
    return state_->status != SharedState::Status::kStarting;
  });
  if (state_->status == SharedState::Status::kFailed) {
    const std::exception_ptr failure = state_->startup_exception;
    std::unique_ptr<std::thread> thread = std::move(bg_thread_);
    lock.unlock();
    if (thread != nullptr && thread->joinable()) {
      thread->join();
    }
    std::rethrow_exception(failure);
  }
  return state_->loop;
}

void Thread::ThreadFunc(const std::shared_ptr<SharedState> &state) noexcept {
  try {
    EventLoop loop;
    if (state->callback) {
      state->callback(&loop);
    }

    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->stop_requested) {
        state->startup_exception = std::make_exception_ptr(
            std::logic_error("Thread stopped during startup"));
        state->status = SharedState::Status::kFailed;
        state->condition.notify_all();
        return;
      }
      state->loop = &loop;
      state->status = SharedState::Status::kRunning;
      state->condition.notify_all();
    }
    loop.Run();
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->loop = nullptr;
      state->status = SharedState::Status::kStopped;
      state->condition.notify_all();
    }
  } catch (...) {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->loop = nullptr;
    if (state->status == SharedState::Status::kStarting) {
      state->startup_exception = std::current_exception();
      state->status = SharedState::Status::kFailed;
    } else {
      state->status = SharedState::Status::kStopped;
    }
    state->condition.notify_all();
  }
}
}  // namespace zrpc
