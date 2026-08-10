#pragma once

#include <string>
#include <string.h>
#include <mutex>
#include <memory>

#ifndef _WIN32
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#else
#include <io.h>
#include <sys/stat.h>
#endif

#include <filesystem>

#include "zrpc/base/timer.h"
namespace zrpc {
const int32_t kSmallBuffer = 1024;
const int32_t kLargeBuffer = 1024 * 64;

template <int32_t SIZE>
class FixedBuffer {
 public:
  FixedBuffer() : cur_(data_) {}

  ~FixedBuffer() {}

  void Append(const char *buf, size_t len) {
    if (Avail() >= len) {
      memcpy(cur_, buf, len);
      cur_ += len;
    }
  }

  const char *GetData() const { return data_; }
  int32_t Length() const { return static_cast<int32_t>(cur_ - data_); }
  char *Current() { return cur_; }
  void Add(size_t len) { cur_ += len; }
  void Reset() { cur_ = data_; }
  void Bzero() { memset(data_, 0, sizeof data_); }

  std::string ToString() const { return std::string(data_, Length()); }
  size_t Avail() const { return static_cast<size_t>(End() - cur_); }
  const char *End() const { return data_ + sizeof data_; }

  std::string_view ToStringView() const {
    return std::string_view(data_, Length());
  }

 private:
  FixedBuffer(const FixedBuffer &);

  void operator=(const FixedBuffer &);

  char data_[SIZE];
  char *cur_;
};

class AppendFile {
 public:
  explicit AppendFile(const std::string &filename);

  ~AppendFile();

  void Append(const char *logline, const size_t len);
  void Flush();
  bool Exists(const std::string &filename);
  bool Valid() const { return fp_ != nullptr; }
  size_t GetWrittenBytes() const { return written_bytes_; }

  int32_t LockOrUnlock(int32_t fd, bool lock);
  bool LockFile(const std::string &fname);
  bool UnlockFile();

 private:
  AppendFile(const AppendFile &);

  void operator=(const AppendFile &);

  size_t Write(const char *logline, size_t len);

  FILE *fp_;
  char buffer_[64 * 1024];
  size_t written_bytes_;
  int32_t fd_;
  const std::string file_name_;
};

class LogFile {
 public:
  LogFile(const std::string &file_path, const std::string &base_name,
          size_t roll_size, bool thread_safe = true, int32_t interval = 3,
          int32_t check_every = 1024);

  ~LogFile();

  void Append(const char *logline, int32_t len);
  void Flush();
  bool RollFile(bool mask = false);

 private:
  void AppendUnlocked(const char *logline, int32_t len);
  void GetLogFileName(time_t *now);

  const std::string file_path_;
  const std::string base_name_;
  const size_t roll_size_;
  const int32_t interval_;
  const int32_t check_every_;

  std::string file_name_;
  int32_t count_;
  std::mutex mutex_;
  time_t start_of_period_;
  time_t last_roll_;
  time_t last_flush_;
  std::unique_ptr<AppendFile> file_;
  const static int32_t kRollPerSeconds = 60 * 60 * 24;
};

class T {
 public:
  T(const char *str) : str_(str), len_(static_cast<int32_t>(strlen(str))) {}

  T(const std::string &str)
      : str_(str.data()), len_(static_cast<int32_t>(str.size())) {}

  T(const char *str, unsigned len) : str_(str), len_(len) {}

  const char *str_;
  const unsigned len_;
};

class LogStream {
 public:
  typedef LogStream self;
  typedef FixedBuffer<kSmallBuffer> Buffer;

  self &operator<<(bool v) {
    buffer_.Append(v ? "1" : "0", 1);
    return *this;
  }

  self &operator<<(short);
  self &operator<<(unsigned short);
  self &operator<<(int);
  self &operator<<(unsigned int);
  self &operator<<(long);
  self &operator<<(unsigned long);
  self &operator<<(long long);
  self &operator<<(unsigned long long);
  self &operator<<(const void *);
  self &operator<<(float v) {
    *this << static_cast<double>(v);
    return *this;
  }

  self &operator<<(double);
  self &operator<<(char v) {
    buffer_.Append(&v, 1);
    return *this;
  }

  self &operator<<(const char *str) {
    if (str) {
      buffer_.Append(str, strlen(str));
    } else {
      buffer_.Append("(null)", 6);
    }
    return *this;
  }

  self &operator<<(const unsigned char *str) {
    return operator<<(reinterpret_cast<const char *>(str));
  }

  self &operator<<(const std::string &v) {
    buffer_.Append(v.c_str(), v.size());
    return *this;
  }

  self &operator<<(const std::string_view &v) {
    buffer_.Append(v.data(), v.size());
    return *this;
  }

  self &operator<<(const Buffer &v) {
    *this << v.ToStringView();
    return *this;
  }

  self &operator<<(const T &v) {
    buffer_.Append(v.str_, v.len_);
    return *this;
  }

  void Append(const char *data, int32_t len) { buffer_.Append(data, len); }
  const Buffer &GetBuffer() const { return buffer_; }
  void ResetBuffer() { buffer_.Reset(); }

 public:
  template <typename T>
  void FormatInteger(T);

  Buffer buffer_;
  static const int32_t kMaxNumericSize = 32;
};

}  // namespace zrpc
