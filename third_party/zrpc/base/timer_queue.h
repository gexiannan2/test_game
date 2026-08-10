#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <functional>
#include <string>
#include <vector>

#include "zrpc/base/timer.h"
#include "zrpc/net/channel.h"

namespace zrpc {

struct TimeoutItem {
  uint64_t expire_us_;
  int64_t seq_;
  int64_t pid_;
};

inline uint64_t NowMicros() {
  return static_cast<uint64_t>(TimeStamp::SteadyNowMicros());
}

struct TimeoutItemGreater {
  bool operator()(const TimeoutItem& a, const TimeoutItem& b) const {
    return a.expire_us_ > b.expire_us_; // expire 更小的优先（在 top）
  }
};

struct TimeoutItemCmp {
  bool operator()(const TimeoutItem& a, const TimeoutItem& b) const {
    if (a.expire_us_ != b.expire_us_) return a.expire_us_ < b.expire_us_;
    return a.seq_ < b.seq_;
  }
};

inline const std::string RPC_TIMEOUT = "rpc_timeout";

class EventLoop;
class TimerQueue {
 public:
  TimerQueue(EventLoop *loop);

  ~TimerQueue();

  void CancelTimer(const std::shared_ptr<Timer> &timer);
  void HandleRead();

  std::shared_ptr<Timer> AddTimer(double when, bool repeat, TimerCallback &&cb);
  std::shared_ptr<Timer> AddTimer(TimeStamp &&stamp, double when, bool repeat,
                                  TimerCallback &&cb);
  std::shared_ptr<Timer> GetTimerBegin();
  int64_t GetTimeout() const;
  size_t GetTimerSize();

 private:
  TimerQueue(const TimerQueue &);

  void operator=(const TimerQueue &);

  EventLoop *loop_;
  int32_t timerfd_;
#ifdef __linux__
  Channel timerfd_channel_;
#endif

  void CancelInloop(const std::shared_ptr<Timer> &timer);
  void AddTimerInLoop(const std::shared_ptr<Timer> &timer);
  void GetExpired(const TimeStamp &now);
  void Reset(const TimeStamp &now);
  bool Insert(const std::shared_ptr<Timer> &timer);

  typedef std::multimap<int64_t, std::shared_ptr<Timer>> TimerList; // ���ж�ʱ����һ�δ���ʱ��(������ͬ)
  typedef std::map<int64_t, std::shared_ptr<Timer>> ActiveTimer; // �������ж�ʱ�� 

  ActiveTimer active_timers_;
  ActiveTimer canceling_timers_;
  TimerList expired_;
  TimerList timers_;
  bool calling_expired_timers_;
};
}  // namespace zrpc
