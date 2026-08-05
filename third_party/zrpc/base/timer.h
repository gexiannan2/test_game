#pragma once

#include <atomic>
#include <cstdint>

#include "zrpc/base/time_stamp.h"
#include "zrpc/net/callback.h"

namespace zrpc {
class Timer {
 public:
  Timer(TimerCallback &&cb, TimeStamp &&expiration, bool repeat,
        double interval);

  ~Timer() = default;

  void Run();
  int64_t GetSequence() const noexcept;
  int64_t GetWhen() const noexcept;
  const TimeStamp &GetExpiration() const noexcept;
  bool GetRepeat() const noexcept;
  void Restart(const TimeStamp &now);
  double GetInterval() const noexcept;

 private:
  Timer(const Timer &) = delete;
  Timer &operator=(const Timer &) = delete;

  bool repeat_;
  double interval_;
  int64_t sequence_;
  TimeStamp expiration_;
  TimerCallback callback_;
  static std::atomic<int64_t> g_num_created;
};
}  // namespace zrpc
