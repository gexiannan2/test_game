#pragma once

#include <functional>
#include <memory>
#include <thread>

namespace zrpc {
class EventLoop;

class Thread {
 public:
  typedef std::function<void(EventLoop *)> ThreadInitCallback;

  Thread(const ThreadInitCallback &cb = ThreadInitCallback());

  ~Thread();

  EventLoop *StartLoop();
  void StopLoop();

 private:
  struct SharedState;

  Thread(const Thread &) = delete;
  Thread &operator=(const Thread &) = delete;

  static void ThreadFunc(const std::shared_ptr<SharedState> &state) noexcept;

  std::shared_ptr<SharedState> state_;
  std::unique_ptr<std::thread> bg_thread_;
};
}  // namespace zrpc
