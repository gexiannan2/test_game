#include "zrpc/base/time_stamp.h"

namespace zrpc {
std::string TimeStamp::ToString() const {
  char buf[32];
  int64_t seconds = micro_seconds_since_epoch_ / kMicroSecondsPerSecond;
  int64_t microseconds = micro_seconds_since_epoch_ % kMicroSecondsPerSecond;
  snprintf(buf, sizeof(buf) - 1, "%" PRId64 ".%06" PRId64 "", seconds,
           microseconds);
  return buf;
}

std::string TimeStamp::ToFormattedString(bool showMicroseconds) const {
  char buf[96];
  time_t seconds =
      static_cast<time_t>(micro_seconds_since_epoch_ / kMicroSecondsPerSecond);
  struct tm tm = {};
#ifdef _WIN32
  localtime_s(&tm, &seconds);
#else
  localtime_r(&seconds, &tm);
#endif
  if (showMicroseconds) {
    int microseconds =
        static_cast<int>(micro_seconds_since_epoch_ % kMicroSecondsPerSecond);
    snprintf(buf, sizeof(buf), "%4d%02d%02d %02d:%02d:%02d.%06d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
             tm.tm_min, tm.tm_sec, microseconds);
  } else {
    snprintf(buf, sizeof(buf), "%4d%02d%02d %02d:%02d:%02d", tm.tm_year + 1900,
             tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
  }
  return buf;
}
}  // namespace zrpc
