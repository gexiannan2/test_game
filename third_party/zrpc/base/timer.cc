#include "zrpc/base/timer.h"

#include <cmath>
#include <stdexcept>

#include "zrpc/base/logger.h"

namespace zrpc {
std::atomic<int64_t> Timer::g_num_created{0};

Timer::Timer(TimerCallback &&cb, TimeStamp &&expiration, bool repeat,
             double interval)
    : repeat_(repeat),
      interval_(interval),
      sequence_(g_num_created.fetch_add(1, std::memory_order_relaxed) + 1),
      expiration_(std::move(expiration)),
      callback_(std::move(cb)) {
  if (!callback_) {
    throw std::invalid_argument("Timer callback must not be empty");
  }
  if (!std::isfinite(interval_) || interval_ < 0.0 ||
      (repeat_ && interval_ <= 0.0)) {
    throw std::invalid_argument("Timer interval is invalid");
  }
  if (!expiration_.Valid()) {
    throw std::invalid_argument("Timer expiration must be valid");
  }
}

int64_t Timer::GetSequence() const noexcept { return sequence_; }

const TimeStamp &Timer::GetExpiration() const noexcept { return expiration_; }

int64_t Timer::GetWhen() const noexcept {
  return expiration_.GetMicroSecondsSinceEpoch();
}

bool Timer::GetRepeat() const noexcept { return repeat_; }

double Timer::GetInterval() const noexcept { return interval_; }

void Timer::Run() {
  try {
    callback_();
  } catch (const std::exception &e) {
    LOG_SYSERR << "Timer callback exception: " << e.what();
  } catch (...) {
    LOG_SYSERR << "Timer callback exception: unknown";
  }
}

void Timer::Restart(const TimeStamp &now) {
  if (repeat_) {
    expiration_ = AddTime(now, interval_);
  } else {
    expiration_ = TimeStamp::Invalid();
  }
}
}  // namespace zrpc
