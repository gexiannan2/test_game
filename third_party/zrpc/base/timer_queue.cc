#include "zrpc/base/timer_queue.h"

#include <cerrno>
#include <cmath>
#include <cstring>
#include <limits>
#include <system_error>

#include "zrpc/base/logger.h"
#include "zrpc/net/event_loop.h"

namespace zrpc {
#if defined(__APPLE__) || defined(_WIN32)
TimerQueue::TimerQueue(EventLoop *loop_)
    : loop_(loop_), timerfd_(-1), calling_expired_timers_(false) {}
#endif

#ifdef __linux__
int32_t CreateTimerfd() {
  const int32_t timer_fd =
      ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (timer_fd < 0) {
    throw std::system_error(errno, std::generic_category(),
                            "timerfd_create failed");
  }
  return timer_fd;
}
#endif

int64_t HowMuchTimeFrom(const TimeStamp &when) {
  int64_t microseconds =
      when.GetMicroSecondsSinceEpoch() - TimeStamp::SteadyNowMicros();
  if (microseconds < 1000) {
    microseconds = 1000;
  }
  const int64_t milliseconds = microseconds / 1000;
  return std::min<int64_t>(milliseconds,
                           std::numeric_limits<int32_t>::max());
}

int64_t TimerQueue::GetTimeout() const {
  loop_->AssertInLoopThread();
  if (timers_.empty()) {
    return 1000;
  } else {
    return HowMuchTimeFrom(timers_.begin()->second->GetExpiration());
  }
}

#ifdef __linux__
struct timespec HowMuchTimeFromNow(const TimeStamp &when) {
  int64_t microseconds =
      when.GetMicroSecondsSinceEpoch() - TimeStamp::SteadyNowMicros();
  if (microseconds < 100) {
    microseconds = 100;
  }

  struct timespec ts;
  ts.tv_sec =
      static_cast<time_t>(microseconds / TimeStamp::kMicroSecondsPerSecond);
  ts.tv_nsec = static_cast<int64_t>(
      (microseconds % TimeStamp::kMicroSecondsPerSecond) * 1000);
  return ts;
}

bool ResetTimerfd(int32_t timerfd, const TimeStamp &expiration) {
  struct itimerspec new_value = {};
  struct itimerspec old_value = {};
  new_value.it_value = HowMuchTimeFromNow(expiration);
  if (::timerfd_settime(timerfd, 0, &new_value, &old_value) != 0) {
    const int saved_errno = errno;
    LOG_SYSERR << "timerfd_settime failed, errno=" << saved_errno;
    return false;
  }
  return true;
}

void ReadTimerfd(int32_t timerfd) {
  uint64_t howmany = 0;
  ssize_t n = 0;
  do {
    n = socket::Read(timerfd, &howmany, sizeof howmany);
  } while (n < 0 && errno == EINTR);
  if (n != static_cast<ssize_t>(sizeof howmany) &&
      !(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) {
    const int saved_errno = errno;
    LOG_SYSERR << "timerfd read failed, bytes=" << n
               << " errno=" << saved_errno;
  }
}
#endif

#ifdef __linux__
TimerQueue::TimerQueue(EventLoop *loop_)
    : loop_(loop_),
      timerfd_(CreateTimerfd()),
      timerfd_channel_(loop_, timerfd_),
      calling_expired_timers_(false) {
  timerfd_channel_.SetReadCallback(std::bind(&TimerQueue::HandleRead, this));
  timerfd_channel_.EnableReading();
}
#endif

TimerQueue::~TimerQueue() {
#ifdef __linux__
  timerfd_channel_.DisableAll();
  timerfd_channel_.Remove();
  ::close(timerfd_);
#endif
}

std::shared_ptr<Timer> TimerQueue::AddTimer(double when, bool repeat,
                                            TimerCallback &&cb) {
  if (!cb || !std::isfinite(when) || when < 0.0 ||
      (repeat && when <= 0.0)) {
    LOG_WARN << "Reject invalid timer: interval=" << when
             << " repeat=" << repeat;
    return nullptr;
  }
  TimeStamp time;
  try {
    time = AddTime(TimeStamp::SteadyNow(), when);
  } catch (const std::exception &e) {
    LOG_WARN << "Reject timer with out-of-range interval: " << e.what();
    return nullptr;
  }
  auto timer =
      std::make_shared<Timer>(std::move(cb), std::move(time), repeat, when);
  loop_->RunInLoop(std::bind(&TimerQueue::AddTimerInLoop, this, timer));
  return timer;
}

std::shared_ptr<Timer> TimerQueue::AddTimer(TimeStamp &&stamp, double when,
                                            bool repeat, TimerCallback &&cb) {
  if (!cb || !stamp.Valid() || !std::isfinite(when) || when < 0.0 ||
      (repeat && when <= 0.0)) {
    LOG_WARN << "Reject invalid absolute timer: interval=" << when
             << " repeat=" << repeat;
    return nullptr;
  }
  const double delay =
      std::max(0.0, TimeDifference(stamp, TimeStamp::Now()));
  TimeStamp monotonic_stamp;
  try {
    monotonic_stamp = AddTime(TimeStamp::SteadyNow(), delay);
  } catch (const std::exception &e) {
    LOG_WARN << "Reject timer with out-of-range expiration: " << e.what();
    return nullptr;
  }
  auto timer = std::make_shared<Timer>(
      std::move(cb), std::move(monotonic_stamp), repeat, when);
  loop_->RunInLoop(std::bind(&TimerQueue::AddTimerInLoop, this, timer));
  return timer;
}

void TimerQueue::CancelTimer(const std::shared_ptr<Timer> &timer) {
  if (!timer) {
    return;
  }
  loop_->RunInLoop(std::bind(&TimerQueue::CancelInloop, this, timer));
}

void TimerQueue::CancelInloop(const std::shared_ptr<Timer> &timer) {
  loop_->AssertInLoopThread();
  assert(timers_.size() == active_timers_.size());

  auto it = active_timers_.find(timer->GetSequence());
  if (it != active_timers_.end()) {
    const int64_t when = timer->GetWhen();
    auto iter = timers_.lower_bound(when);
    while (iter != timers_.end() && iter->first == when) {
      if (timer->GetSequence() == iter->second->GetSequence()) {
        timers_.erase(iter);
        break;
      }
      ++iter;
    }
    active_timers_.erase(it);
  } else if (calling_expired_timers_) {
    canceling_timers_.insert(std::make_pair(timer->GetSequence(), timer));
  }
  assert(timers_.size() == active_timers_.size());
}

void TimerQueue::AddTimerInLoop(const std::shared_ptr<Timer> &timer) {
  loop_->AssertInLoopThread();
  bool earliest_changed = Insert(timer);
  if (earliest_changed) {
#ifdef __linux__
    ResetTimerfd(timerfd_, timer->GetExpiration());
#endif
  }
}

std::shared_ptr<Timer> TimerQueue::GetTimerBegin() {
  if (timers_.empty()) {
    return nullptr;
  }
  return timers_.begin()->second;
}

void TimerQueue::HandleRead() {
  loop_->AssertInLoopThread();
  assert(timers_.size() == active_timers_.size());
  TimeStamp now(TimeStamp::SteadyNowMicros());

#ifdef __linux__
  ReadTimerfd(timerfd_);
#endif
  GetExpired(now);

  calling_expired_timers_ = true;
  canceling_timers_.clear();

  for (auto &it : expired_) {
    it.second->Run();
  }

  calling_expired_timers_ = false;
  Reset(now);
}

bool TimerQueue::Insert(const std::shared_ptr<Timer> &timer) {
  loop_->AssertInLoopThread();
  assert(timers_.size() == active_timers_.size());

  bool earliest_changed = false;
  int64_t now_time = timer->GetExpiration().GetMicroSecondsSinceEpoch();
  auto it = timers_.begin();
  if (it == timers_.end() || now_time < it->first) {
    earliest_changed = true;
  }

  timers_.insert(std::make_pair(now_time, timer));
  active_timers_.insert(std::make_pair(timer->GetSequence(), timer));
  assert(timers_.size() == active_timers_.size());
  return earliest_changed;
}

void TimerQueue::Reset(const TimeStamp &now) {
  TimeStamp next_expire;
  for (auto &it : expired_) {
    if (it.second->GetRepeat() &&
        canceling_timers_.find(it.second->GetSequence()) ==
            canceling_timers_.end()) {
      try {
        it.second->Restart(now);
        Insert(it.second);
      } catch (const std::exception &e) {
        LOG_WARN << "Drop repeating timer with invalid expiration: "
                 << e.what();
      }
    }
  }

  expired_.clear();
  if (!timers_.empty()) {
    next_expire = timers_.begin()->second->GetExpiration();
  }

  if (next_expire.Valid()) {
#ifdef __linux__
    ResetTimerfd(timerfd_, next_expire);
#endif
  }
}

size_t TimerQueue::GetTimerSize() {
  loop_->AssertInLoopThread();
  assert(timers_.size() == active_timers_.size());
  return timers_.size();
}

void TimerQueue::GetExpired(const TimeStamp &now) {
  assert(timers_.size() == active_timers_.size());
  int64_t now_time = now.GetMicroSecondsSinceEpoch();
  auto end = timers_.upper_bound(now_time);
  assert(end == timers_.end() || now_time < end->first);
  expired_.insert(timers_.begin(), end);
  timers_.erase(timers_.begin(), end);

  for (auto &it : expired_) {
    size_t n = active_timers_.erase(it.second->GetSequence());
    assert(n == 1);
    (void)n;
  }
  assert(timers_.size() == active_timers_.size());
}

}  // namespace zrpc
