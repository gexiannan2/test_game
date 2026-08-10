#pragma once

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <functional>

#include "zrpc/base/log.h"

namespace zrpc {
class Logger {
 public:
  enum LogLevel {
    TRACE,
    DEBUG,
    INFO,
    WARN,
    FATAL,
    ERR,
    NUM_LOG_LEVELS,
  };

  class SourceFile {
   public:
    template <int32_t N>
    inline SourceFile(const char (&arr)[N]) : data_(arr), size_(N - 1) {
      const char *slash = strrchr(data_, '/');
      const char *backslash = strrchr(data_, '\\');
      if (backslash != nullptr &&
          (slash == nullptr || backslash > slash)) {
        slash = backslash;
      }
      if (slash) {
        data_ = slash + 1;
        size_ -= static_cast<int32_t>(data_ - arr);
      }
    }

    explicit SourceFile(const char *file_name) : data_(file_name) {
      const char *slash = strrchr(file_name, '/');
      const char *backslash = strrchr(file_name, '\\');
      if (backslash != nullptr &&
          (slash == nullptr || backslash > slash)) {
        slash = backslash;
      }
      if (slash) {
        data_ = slash + 1;
      }
      size_ = static_cast<int32_t>(strlen(data_));
    }

    const char *data_;
    int32_t size_;
  };

  Logger(SourceFile file, int32_t line);
  Logger(SourceFile file, int32_t line, LogLevel level);
  Logger(SourceFile file, int32_t line, LogLevel level,
         int32_t saved_errno);
  Logger(SourceFile file, int32_t line, LogLevel level, const char *func);
  ~Logger();

  LogStream &Stream() { return impl_.stream_; }
  static LogLevel GetLogLevel();
  static void SetLogLevel(LogLevel level);
  typedef std::function<void(const char *msg, int32_t len)> OutputFunc;
  typedef std::function<void()> FlushFunc;
  static void SetOutput(const OutputFunc &func);
  static void SetFlush(const FlushFunc &func);

 private:
  class Impl {
   public:
    typedef Logger::LogLevel LogLevel;

    Impl(LogLevel level, int32_t old_errno, const SourceFile &file,
         int32_t line);

    void FormatTime();

    void Finish();

    LogStream stream_;
    LogLevel level_;
    int32_t line_;
    SourceFile base_name_;
    TimeStamp time_;
  };

  Impl impl_;
};

extern std::atomic<Logger::LogLevel> g_log_level;

inline Logger::LogLevel Logger::GetLogLevel() {
  return g_log_level.load(std::memory_order_relaxed);
}

#define LOG_TRACE                                                        \
  if (zrpc::Logger::GetLogLevel() > zrpc::Logger::TRACE) {               \
  } else                                                                 \
    zrpc::Logger(__FILE__, __LINE__, zrpc::Logger::TRACE, __func__).Stream()
#define LOG_DEBUG                                                        \
  if (zrpc::Logger::GetLogLevel() > zrpc::Logger::DEBUG) {               \
  } else                                                                 \
    zrpc::Logger(__FILE__, __LINE__, zrpc::Logger::DEBUG, __func__).Stream()
#define LOG_INFO                                                   \
  if (zrpc::Logger::GetLogLevel() > zrpc::Logger::INFO) {          \
  } else                                                           \
    zrpc::Logger(__FILE__, __LINE__).Stream()
#define LOG_WARN                                                   \
  if (zrpc::Logger::GetLogLevel() > zrpc::Logger::WARN) {          \
  } else                                                           \
    zrpc::Logger(__FILE__, __LINE__, zrpc::Logger::WARN).Stream()
#define LOG_ERROR                                                   \
  if (zrpc::Logger::GetLogLevel() > zrpc::Logger::ERR) {            \
  } else                                                           \
    zrpc::Logger(__FILE__, __LINE__, zrpc::Logger::ERR).Stream()
#define LOG_FATAL \
  zrpc::Logger(__FILE__, __LINE__, zrpc::Logger::FATAL).Stream()
#define LOG_SYSERR \
  zrpc::Logger(__FILE__, __LINE__, zrpc::Logger::ERR, errno).Stream()
#define LOG_SYSFATAL \
  zrpc::Logger(__FILE__, __LINE__, zrpc::Logger::FATAL, errno).Stream()
}  // namespace zrpc
