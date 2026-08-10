#include "zrpc/base/log.h"

#include <stdexcept>

namespace zrpc {
namespace {
std::string MakeLogPath(const std::string &directory,
                        const std::string &file_name) {
  return (std::filesystem::path(directory) / file_name).string();
}
}

const char digits[] = "9876543210123456789";
const char digitsHex[] = "0123456789ABCDEF";
const char *zero = digits + 9;
char ttime[32];

template <typename T>
size_t convert(char buf[], T value) {
  T i = value;
  char *p = buf;

  do {
    int32_t lsd = static_cast<int32_t>(i % 10);
    i /= 10;
    *p++ = zero[lsd];
  } while (i != 0);

  if (value < 0) {
    *p++ = '-';
  }

  *p = '\0';
  std::reverse(buf, p);
  return p - buf;
}

size_t ConvertHex(char buf[], uintptr_t value) {
  uintptr_t i = value;
  char *p = buf;

  do {
    int32_t lsd = static_cast<int32_t>(i % 16);
    i /= 16;
    *p++ = digitsHex[lsd];
  } while (i != 0);

  *p = '\0';
  std::reverse(buf, p);
  return p - buf;
}

AppendFile::AppendFile(const std::string &file_name)
    : fp_(nullptr), written_bytes_(0), fd_(-1), file_name_(file_name) {
#ifdef _WIN32
  fp_ = ::fopen(file_name_.c_str(), "at+");
#else
  fp_ = ::fopen(file_name_.c_str(), "ae");
#endif
}

AppendFile::~AppendFile() {
  if (fp_ != nullptr) {
    ::fclose(fp_);
    fp_ = nullptr;
  }
#ifndef _WIN32
  if (fd_ >= 0) {
    LockOrUnlock(fd_, false);
    ::close(fd_);
    fd_ = -1;
  }
#endif
}

bool AppendFile::Exists(const std::string &file_name) {
  std::error_code ec;
  return std::filesystem::exists(file_name, ec);
}

bool AppendFile::LockFile(const std::string &file_name) {
#ifndef _WIN32
  if (fd_ >= 0 && !UnlockFile()) {
    return false;
  }
  int32_t fd = ::open(file_name.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd < 0) {
    return false;
  }
  if (LockOrUnlock(fd, true) == -1) {
    ::close(fd);
    return false;
  }
  fd_ = fd;
#endif
  return true;
}

bool AppendFile::UnlockFile() {
#ifndef _WIN32
  if (fd_ < 0) {
    return false;
  }
  if (LockOrUnlock(fd_, false) == -1) {
    return false;
  }
  ::close(fd_);
  fd_ = -1;
#endif
  return true;
}

int32_t AppendFile::LockOrUnlock(int32_t fd, bool lock) {
#ifndef _WIN32
  struct flock f;
  memset(&f, 0, sizeof(f));
  f.l_type = (lock ? F_WRLCK : F_UNLCK);
  f.l_whence = SEEK_SET;
  f.l_start = 0;
  f.l_len = 0;  // Lock/unlock entire file
  return ::fcntl(fd, F_SETLK, &f);
#else
  return 0;
#endif
}

void AppendFile::Append(const char *logline, const size_t len) {
  if (fp_ == nullptr || len == 0) {
    return;
  }
  size_t n = Write(logline, len);
  size_t remain = len - n;
  while (remain > 0) {
    size_t x = Write(logline + n, remain);
    if (x == 0) {
      int32_t err = ferror(fp_);
      if (err) {
      }
      break;
    }

    n += x;
    remain = len - n;
  }
  written_bytes_ += n;
}

void AppendFile::Flush() {
  if (fp_ != nullptr) {
    ::fflush(fp_);
  }
}

size_t AppendFile::Write(const char *logline, size_t len) {
  if (fp_ == nullptr) {
    return 0;
  }
#ifdef __linux__
  return ::fwrite_unlocked(logline, 1, len, fp_);
#else
  return ::fwrite(logline, 1, len, fp_);
#endif
}

LogFile::LogFile(const std::string &file_path, const std::string &base_name,
                 size_t roll_size, bool, int32_t interval,
                 int32_t check_every)
    : file_path_(file_path),
      base_name_(base_name),
      roll_size_(roll_size),
      interval_(interval),
      check_every_(check_every),
      count_(0),
      start_of_period_(0),
      last_roll_(0),
      last_flush_(0) {
  if (base_name_.empty() ||
      base_name_.find_first_of("/\\") != std::string::npos) {
    throw std::invalid_argument("LogFile base name is invalid");
  }
  if (roll_size_ == 0 || interval_ <= 0 || check_every_ <= 0) {
    throw std::invalid_argument("LogFile rolling parameters are invalid");
  }
  std::error_code ec;
  std::filesystem::create_directories(file_path_, ec);
  RollFile(true);
}

LogFile::~LogFile() {}

void LogFile::Append(const char *logline, int32_t len) {
  std::unique_lock<std::mutex> lk(mutex_);
  AppendUnlocked(logline, len);
}

void LogFile::Flush() {
  std::unique_lock<std::mutex> lk(mutex_);
  if (file_ != nullptr) {
    file_->Flush();
  }
}

void LogFile::AppendUnlocked(const char *logline, int32_t len) {
  if (logline == nullptr || len <= 0) {
    return;
  }
  if (file_ == nullptr || !file_->Valid()) {
    std::error_code ec;
    std::filesystem::create_directories(file_path_, ec);
    if (!RollFile(true) || file_ == nullptr || !file_->Valid()) {
      ::fwrite(logline, 1, static_cast<size_t>(len), stderr);
      return;
    }
  }
  if (!file_->Exists(MakeLogPath(file_path_, base_name_ + ".log"))) {
    std::error_code ec;
    std::filesystem::create_directories(file_path_, ec);
    file_name_.clear();
    count_ = 0;
    start_of_period_ = 0;
    last_roll_ = 0;
    last_flush_ = 0;
    if (!RollFile(true) || file_ == nullptr || !file_->Valid()) {
      ::fwrite(logline, 1, static_cast<size_t>(len), stderr);
      return;
    }
    file_->Append(logline, static_cast<size_t>(len));
    return;
  }

  file_->Append(logline, static_cast<size_t>(len));
  if (file_->GetWrittenBytes() > roll_size_) {
    RollFile();
  } else {
    ++count_;
    if (count_ >= check_every_) {
      count_ = 0;
      time_t now = ::time(0);
      time_t this_period = now / kRollPerSeconds * kRollPerSeconds;
      if (this_period != start_of_period_) {
        RollFile();
      } else if (now - last_flush_ > interval_) {
        last_flush_ = now;
        file_->Flush();
      }
    }
  }
}

bool LogFile::RollFile(bool mask) {
  time_t now = 0;
  GetLogFileName(&now);
  time_t Start = now / kRollPerSeconds * kRollPerSeconds;

  if (now > last_roll_) {
    const std::string active_name =
        MakeLogPath(file_path_, base_name_ + ".log");

    if (file_ != nullptr) {
      file_->Flush();
    }

    if (!mask && !file_name_.empty() && file_ != nullptr && file_->Valid()) {
      file_.reset();
      std::rename(active_name.c_str(), file_name_.c_str());
    } else {
      file_.reset();
    }

    auto new_file = std::make_unique<AppendFile>(active_name);
    if (!new_file->Valid()) {
      return false;
    }

    last_roll_ = now;
    last_flush_ = now;
    start_of_period_ = Start;
    file_ = std::move(new_file);
    return true;
  }
  return false;
}

void LogFile::GetLogFileName(time_t *now) {
  file_name_.clear();
  file_name_ = MakeLogPath(file_path_, base_name_);

  char timebuf[32];
  struct tm tm = {};
  *now = time(0);
#ifdef _WIN32
  localtime_s(&tm, now);
#else
  localtime_r(now, &tm);
#endif
  strftime(timebuf, sizeof timebuf, ".%Y-%m-%d-%H-%M-%S", &tm);
  file_name_ += timebuf;
  file_name_ += ".log";
}

template <typename T>
void LogStream::FormatInteger(T v) {
  if (buffer_.Avail() >= kMaxNumericSize) {
    size_t len = convert(buffer_.Current(), v);
    buffer_.Add(len);
  }
}

LogStream &LogStream::operator<<(short v) {
  *this << static_cast<int>(v);
  return *this;
}

LogStream &LogStream::operator<<(unsigned short v) {
  *this << static_cast<unsigned int>(v);
  return *this;
}

LogStream &LogStream::operator<<(int v) {
  FormatInteger(v);
  return *this;
}

LogStream &LogStream::operator<<(unsigned int v) {
  FormatInteger(v);
  return *this;
}

LogStream &LogStream::operator<<(long v) {
  FormatInteger(v);
  return *this;
}

LogStream &LogStream::operator<<(unsigned long v) {
  FormatInteger(v);
  return *this;
}

LogStream &LogStream::operator<<(long long v) {
  FormatInteger(v);
  return *this;
}

LogStream &LogStream::operator<<(unsigned long long v) {
  FormatInteger(v);
  return *this;
}

LogStream &LogStream::operator<<(const void *p) {
  uintptr_t v = reinterpret_cast<uintptr_t>(p);
  if (buffer_.Avail() >= kMaxNumericSize) {
    char *buf = buffer_.Current();
    buf[0] = '0';
    buf[1] = 'x';
    size_t len = ConvertHex(buf + 2, v);
    buffer_.Add(len + 2);
  }
  return *this;
}

// FIXME: replace this with Grisu3 by Florian Loitsch.
LogStream &LogStream::operator<<(double v) {
  if (buffer_.Avail() >= kMaxNumericSize) {
    int32_t len = snprintf(buffer_.Current(), kMaxNumericSize, "%.12g", v);
    buffer_.Add(len);
  }
  return *this;
}
}  // namespace zrpc
