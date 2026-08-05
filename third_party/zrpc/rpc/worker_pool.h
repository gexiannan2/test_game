#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace zrpc {
namespace rpc {

class WorkerPool {
 public:
  explicit WorkerPool(int num_threads,
                      size_t max_pending_tasks = 1024);
  ~WorkerPool();

  void Start();
  void Stop();
  bool Post(std::function<void()> task);
  bool Active() const;

 private:
  struct State {
    explicit State(size_t max_pending) : max_pending_tasks(max_pending) {}

    const size_t max_pending_tasks;
    bool started = false;
    bool stopped = false;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::queue<std::function<void()>> tasks;
  };

  static void WorkerLoop(const std::shared_ptr<State>& state);

  int num_threads_;
  std::shared_ptr<State> state_;
  mutable std::mutex workers_mutex_;
  std::vector<std::thread> workers_;
};

}  // namespace rpc
}  // namespace zrpc
