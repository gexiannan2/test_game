#include "zrpc/rpc/worker_pool.h"

#include <exception>

#include "zrpc/base/logger.h"

namespace zrpc {
namespace rpc {

WorkerPool::WorkerPool(int num_threads, size_t max_pending_tasks)
    : num_threads_(num_threads > 0 ? num_threads : 0),
      state_(std::make_shared<State>(
          max_pending_tasks > 0 ? max_pending_tasks : 1)) {}

WorkerPool::~WorkerPool() { Stop(); }

void WorkerPool::Start() {
  std::lock_guard<std::mutex> workers_lk(workers_mutex_);
  std::lock_guard<std::mutex> state_lk(state_->mutex);
  if (num_threads_ <= 0 || state_->started || state_->stopped) {
    return;
  }
  state_->started = true;
  workers_.reserve(static_cast<size_t>(num_threads_));
  for (int i = 0; i < num_threads_; ++i) {
    std::shared_ptr<State> state = state_;
    workers_.emplace_back([state]() { WorkerLoop(state); });
  }
}

void WorkerPool::Stop() {
  {
    std::lock_guard<std::mutex> lk(state_->mutex);
    if (!state_->started || state_->stopped) {
      return;
    }
    state_->stopped = true;
  }
  state_->cv.notify_all();

  std::lock_guard<std::mutex> workers_lk(workers_mutex_);
  const std::thread::id current = std::this_thread::get_id();
  bool called_from_worker = false;
  for (const auto& worker : workers_) {
    if (worker.joinable() && worker.get_id() == current) {
      called_from_worker = true;
      break;
    }
  }
  for (auto& worker : workers_) {
    if (!worker.joinable()) {
      continue;
    }
    if (called_from_worker) {
      worker.detach();
    } else {
      worker.join();
    }
  }
  workers_.clear();
  {
    std::lock_guard<std::mutex> lk(state_->mutex);
    state_->started = false;
    if (!called_from_worker) {
      state_->stopped = false;
    }
  }
}

bool WorkerPool::Active() const {
  std::lock_guard<std::mutex> lk(state_->mutex);
  return state_->started && !state_->stopped && num_threads_ > 0;
}

bool WorkerPool::Post(std::function<void()> task) {
  if (!task) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lk(state_->mutex);
    if (!state_->started || state_->stopped ||
        state_->tasks.size() >= state_->max_pending_tasks) {
      return false;
    }
    state_->tasks.push(std::move(task));
  }
  state_->cv.notify_one();
  return true;
}

void WorkerPool::WorkerLoop(const std::shared_ptr<State>& state) {
  while (true) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lk(state->mutex);
      state->cv.wait(
          lk, [&state]() { return state->stopped || !state->tasks.empty(); });
      if (state->stopped && state->tasks.empty()) {
        return;
      }
      task = std::move(state->tasks.front());
      state->tasks.pop();
    }
    try {
      task();
    } catch (const std::exception& ex) {
      LOG_WARN << "rpc worker task threw: " << ex.what();
    } catch (...) {
      LOG_WARN << "rpc worker task threw an unknown exception";
    }
  }
}

}  // namespace rpc
}  // namespace zrpc
