#include "zrpc/base/logger.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <system_error>

namespace zrpc {
Logger::LogLevel InitLogLevel() {
#ifdef _WIN32
  return Logger::INFO;
#else
  if (::getenv("TRACE"))
    return Logger::TRACE;
  else if (::getenv("DEBUG"))
    return Logger::DEBUG;
  else
    return Logger::INFO;
#endif
}

std::atomic<Logger::LogLevel> g_log_level{InitLogLevel()};

const char *LogLevelName[Logger::NUM_LOG_LEVELS] = {
    "TRACE ", "DEBUG ", "INFO  ", "WARN  ", "FATAL ", "ERROR ",
};

void DefaultOutput(const char *msg, int32_t len) {
  size_t n = ::fwrite(msg, 1, len, stdout);
  // FIXME check n
  (void)n;
  fflush(stdout);
}

void DefaultFlush() { fflush(stdout); }

Logger::OutputFunc g_output = DefaultOutput;
Logger::FlushFunc g_flush = DefaultFlush;
std::mutex g_callback_mutex;

Logger::Impl::Impl(LogLevel level, int32_t saved_errno, const SourceFile &file,
                   int32_t line)
    : stream_(),
      level_(level),
      line_(line),
      base_name_(file),
      time_(TimeStamp::NowMicros()) {
  FormatTime();
  stream_ << T(LogLevelName[level], 6);
  if (saved_errno != 0) {
    stream_ << std::error_code(saved_errno, std::generic_category()).message()
            << " (errno=" << saved_errno << ") ";
  }
}

thread_local char t_time[64];
thread_local time_t t_last_second = static_cast<time_t>(-1);

void Logger::Impl::FormatTime() {
  int32_t len = 0;
  int64_t epoch = time_.GetMicroSecondsSinceEpoch();
  time_t seconds =
      static_cast<time_t>(epoch / TimeStamp::kMicroSecondsPerSecond);
  int microseconds =
      static_cast<int>(epoch % TimeStamp::kMicroSecondsPerSecond);
  if (seconds != t_last_second) {
    t_last_second = seconds;
    struct tm tmtime = {};
#ifdef _WIN32
    localtime_s(&tmtime, &seconds);
#else
    localtime_r(&seconds, &tmtime);
#endif
    len = snprintf(t_time, sizeof(t_time), "%4d%02d%02d %02d:%02d:%02d",
                   tmtime.tm_year + 1900, tmtime.tm_mon + 1, tmtime.tm_mday,
                   tmtime.tm_hour, tmtime.tm_min, tmtime.tm_sec);
    assert(len == 17);
    (void)len;
  }

  stream_ << T(t_time, 17);
  char microsecond_text[8] = {};
  const int microsecond_len =
      snprintf(microsecond_text, sizeof(microsecond_text), ".%06d",
               microseconds);
  if (microsecond_len == 7) {
    stream_ << T(microsecond_text, 7);
  }
  stream_ << ' ';
}

void Logger::Impl::Finish() {
  stream_ << " - " << base_name_.data_ << ':' << line_ << '\n';
}

Logger::Logger(SourceFile file, int32_t line) : impl_(INFO, 0, file, line) {}

Logger::Logger(SourceFile file, int32_t line, LogLevel level, const char *func)
    : impl_(level, 0, file, line) {
  impl_.stream_ << func << ' ';
}

Logger::Logger(SourceFile file, int32_t line, LogLevel level)
    : impl_(level, 0, file, line) {}

Logger::Logger(SourceFile file, int32_t line, LogLevel level,
               int32_t saved_errno)
    : impl_(level, saved_errno, file, line) {}

Logger::~Logger() {
  impl_.Finish();
  const LogStream::Buffer &buf(Stream().GetBuffer());
  OutputFunc output;
  FlushFunc flush;
  {
    std::lock_guard<std::mutex> lock(g_callback_mutex);
    output = g_output;
    flush = g_flush;
  }
  try {
    output(buf.GetData(), buf.Length());
  } catch (...) {
    DefaultOutput(buf.GetData(), buf.Length());
  }
  if (impl_.level_ == FATAL) {
    try {
      flush();
    } catch (...) {
      DefaultFlush();
    }
    abort();
  }
}

void Logger::SetLogLevel(Logger::LogLevel level) {
  if (level < TRACE || level >= NUM_LOG_LEVELS) {
    throw std::invalid_argument("Logger level is out of range");
  }
  g_log_level.store(level, std::memory_order_relaxed);
}

void Logger::SetOutput(const OutputFunc &func) {
  std::lock_guard<std::mutex> lock(g_callback_mutex);
  g_output = func ? func : OutputFunc(DefaultOutput);
}

void Logger::SetFlush(const FlushFunc &func) {
  std::lock_guard<std::mutex> lock(g_callback_mutex);
  g_flush = func ? func : FlushFunc(DefaultFlush);
}
}  // namespace zrpc
