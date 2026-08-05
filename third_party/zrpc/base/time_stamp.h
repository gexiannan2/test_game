#pragma once

#include <string>
#include <time.h>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdio.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdexcept>

namespace zrpc {

class TimeStamp {
 public:
  TimeStamp() : micro_seconds_since_epoch_(0) {}

  explicit TimeStamp(int64_t micro_seconds_since_epoch_arg)
      : micro_seconds_since_epoch_(micro_seconds_since_epoch_arg) {}

  int64_t GetMicroSecondsSinceEpoch() const {
    return micro_seconds_since_epoch_;
  }

  time_t SecondsSinceEpoch() const {
    return static_cast<time_t>(micro_seconds_since_epoch_ /
                               kMicroSecondsPerSecond);
  }

  bool Valid() const { return micro_seconds_since_epoch_ > 0; }

  std::string ToFormattedString(bool showMicroseconds = true) const;

  static TimeStamp Now() {
    auto time_now = std::chrono::system_clock::now();
    auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(
        time_now.time_since_epoch());
    return TimeStamp(microseconds.count());
  }

  static int64_t NowMicros() {
    auto time_now = std::chrono::system_clock::now();
    auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(
        time_now.time_since_epoch());
    return microseconds.count();
  }

  static TimeStamp SteadyNow() { return TimeStamp(SteadyNowMicros()); }

  static int64_t SteadyNowMicros() {
    const auto time_now = std::chrono::steady_clock::now();
    const auto microseconds =
        std::chrono::duration_cast<std::chrono::microseconds>(
            time_now.time_since_epoch());
    return microseconds.count();
  }

  std::string ToString() const;
  static TimeStamp Invalid() { return TimeStamp(); }
  static const int32_t kMicroSecondsPerSecond = 1000 * 1000;

 private:
  int64_t micro_seconds_since_epoch_;
};

inline bool operator<(const TimeStamp &lhs, const TimeStamp &rhs) {
  return lhs.GetMicroSecondsSinceEpoch() < rhs.GetMicroSecondsSinceEpoch();
}

inline bool operator==(const TimeStamp &lhs, const TimeStamp &rhs) {
  return lhs.GetMicroSecondsSinceEpoch() == rhs.GetMicroSecondsSinceEpoch();
}

inline TimeStamp AddTime(const TimeStamp &timestamp, double seconds) {
  if (!std::isfinite(seconds)) {
    throw std::invalid_argument("TimeStamp delta must be finite");
  }
  const long double delta_value =
      static_cast<long double>(seconds) *
      TimeStamp::kMicroSecondsPerSecond;
  if (delta_value >
          static_cast<long double>(std::numeric_limits<int64_t>::max()) ||
      delta_value <
          static_cast<long double>(std::numeric_limits<int64_t>::min())) {
    throw std::overflow_error("TimeStamp delta is out of range");
  }
  const int64_t delta = static_cast<int64_t>(delta_value);
  const int64_t value = timestamp.GetMicroSecondsSinceEpoch();
  if ((delta > 0 &&
       value > std::numeric_limits<int64_t>::max() - delta) ||
      (delta < 0 &&
       value < std::numeric_limits<int64_t>::min() - delta)) {
    throw std::overflow_error("TimeStamp addition overflow");
  }
  return TimeStamp(value + delta);
}

inline double TimeDifference(const TimeStamp &high, const TimeStamp &low) {
  return (static_cast<double>(high.GetMicroSecondsSinceEpoch()) -
          static_cast<double>(low.GetMicroSecondsSinceEpoch())) /
         TimeStamp::kMicroSecondsPerSecond;
}

}  // namespace zrpc
