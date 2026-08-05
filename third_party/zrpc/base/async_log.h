#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <functional>
#include <thread>
#include <vector>
#include <mutex>
#include <condition_variable>

#include "zrpc/base/logger.h"

namespace zrpc {

class AsyncLogging {
 public:
  AsyncLogging(std::string file_path, std::string base_name, size_t roll_size,
               int32_t interval = 3);

  ~AsyncLogging();

  void Stop();

  void Start();

  void Append(const char *loline, size_t len);

 private:
  AsyncLogging(const AsyncLogging &);

  void operator=(const AsyncLogging &);

  void ThreadFunc() noexcept;
  void ThreadFuncImpl();

  typedef FixedBuffer<kLargeBuffer> Buffer;
  typedef std::vector<std::unique_ptr<Buffer>> BufferVector;
  std::string file_path_;
  std::string base_name_;
  const int32_t interval_;
  std::atomic<bool> running_{false};
  size_t roll_size_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::thread thread_;
  std::unique_ptr<Buffer> current_buffer_;
  std::unique_ptr<Buffer> next_buffer_;
  BufferVector buffers_;
};

}  // namespace zrpc
