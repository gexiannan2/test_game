#include "zrpc/base/async_log.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <exception>
#include <stdexcept>

namespace zrpc {
AsyncLogging::AsyncLogging(std::string file_path, std::string base_name,
                           size_t roll_size, int32_t interval_)
    : file_path_(file_path),
      base_name_(base_name),
      interval_(std::max(1, interval_)),
      roll_size_(std::max<size_t>(1, roll_size)),
      current_buffer_(new Buffer),
      next_buffer_(new Buffer),
      buffers_() {
  current_buffer_->Bzero();
  next_buffer_->Bzero();
  buffers_.reserve(16);
}

AsyncLogging::~AsyncLogging() { Stop(); }

void AsyncLogging::Stop() {
  {
    std::unique_lock<std::mutex> lk(mutex_);
    running_.store(false, std::memory_order_release);
  }
  condition_.notify_one();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void AsyncLogging::Start() {
  std::unique_lock<std::mutex> lk(mutex_);
  if (running_.load(std::memory_order_acquire)) {
    return;
  }
  if (thread_.joinable()) {
    if (thread_.get_id() == std::this_thread::get_id()) {
      throw std::logic_error("AsyncLogging cannot restart from its worker");
    }
    lk.unlock();
    thread_.join();
    lk.lock();
  }
  running_.store(true, std::memory_order_release);
  try {
    thread_ = std::thread(&AsyncLogging::ThreadFunc, this);
  } catch (...) {
    running_.store(false, std::memory_order_release);
    throw;
  }
}

void AsyncLogging::Append(const char *logline, size_t len) {
  if (logline == nullptr || len == 0) {
    return;
  }
  std::unique_lock<std::mutex> lk(mutex_);
  if (!running_.load(std::memory_order_acquire)) {
    return;
  }

  if (len <= current_buffer_->Avail()) {
    current_buffer_->Append(logline, len);
    return;
  }

  auto rotate_buffer = [this]() {
    buffers_.push_back(std::unique_ptr<Buffer>(current_buffer_.release()));
    if (next_buffer_) {
      current_buffer_ = std::move(next_buffer_);
    } else {
      current_buffer_.reset(new Buffer);
    }
    condition_.notify_one();
  };

  if (len <= kLargeBuffer) {
    rotate_buffer();
    current_buffer_->Append(logline, len);
    return;
  }

  while (len > 0) {
    if (current_buffer_->Avail() == 0) {
      rotate_buffer();
    }
    const size_t chunk = std::min(len, current_buffer_->Avail());
    current_buffer_->Append(logline, chunk);
    logline += chunk;
    len -= chunk;
  }
}

void AsyncLogging::ThreadFunc() noexcept {
  try {
    ThreadFuncImpl();
  } catch (const std::exception &e) {
    std::fprintf(stderr, "AsyncLogging worker stopped: %s\n", e.what());
    running_.store(false, std::memory_order_release);
    condition_.notify_all();
  } catch (...) {
    std::fputs("AsyncLogging worker stopped: unknown exception\n", stderr);
    running_.store(false, std::memory_order_release);
    condition_.notify_all();
  }
}

void AsyncLogging::ThreadFuncImpl() {
  LogFile output(file_path_, base_name_, roll_size_, false);
  std::unique_ptr<Buffer> new_buffer1(new Buffer);
  std::unique_ptr<Buffer> new_buffer2(new Buffer);
  new_buffer1->Bzero();
  new_buffer2->Bzero();

  BufferVector buffers_to_write;
  buffers_to_write.reserve(16);

  while (running_.load(std::memory_order_acquire)) {
    assert(new_buffer1 && new_buffer1->Length() == 0);
    assert(new_buffer2 && new_buffer2->Length() == 0);
    assert(buffers_to_write.empty());
    {
      std::unique_lock<std::mutex> lk(mutex_);
      if (buffers_.empty()) {
        condition_.wait_for(lk, std::chrono::seconds(interval_), [this]() {
          return !buffers_.empty() ||
                 !running_.load(std::memory_order_acquire);
        });
      }

      buffers_.push_back(std::unique_ptr<Buffer>(current_buffer_.release()));
      current_buffer_ = std::move(new_buffer1);
      buffers_to_write.swap(buffers_);

      if (!next_buffer_) {
        next_buffer_ = std::move(new_buffer2);
      }
    }

    if (buffers_to_write.empty()) {
      continue;
    }

    if (buffers_to_write.size() > 25) {
      char buf[256];
      snprintf(buf, sizeof buf,
               "Dropped log messages at %s, %zd larger buffers_\n",
               TimeStamp::Now().ToFormattedString().c_str(),
               buffers_to_write.size() - 2);
      fputs(buf, stderr);
      output.Append(buf, static_cast<int>(::strlen(buf)));
      buffers_to_write.erase(buffers_to_write.begin() + 2,
                             buffers_to_write.end());
    }

    for (size_t i = 0; i < buffers_to_write.size(); ++i) {
      output.Append(buffers_to_write[i]->GetData(),
                    buffers_to_write[i]->Length());
    }

    if (buffers_to_write.size() > 2) {
      buffers_to_write.resize(2);
    }

    if (!new_buffer1) {
      assert(!buffers_to_write.empty());
      new_buffer1 = std::move(buffers_to_write.back());
      buffers_to_write.pop_back();
      new_buffer1->Reset();
    }

    if (!new_buffer2) {
      assert(!buffers_to_write.empty());
      new_buffer2 = std::move(buffers_to_write.back());
      buffers_to_write.pop_back();
      new_buffer2->Reset();
    }

    buffers_to_write.clear();
    output.Flush();
  }

  {
    std::unique_lock<std::mutex> lk(mutex_);
    if (current_buffer_ && current_buffer_->Length() > 0) {
      buffers_to_write.push_back(std::unique_ptr<Buffer>(current_buffer_.release()));
      current_buffer_.reset(new Buffer);
      current_buffer_->Bzero();
    }
    for (auto& buf : buffers_) {
      buffers_to_write.push_back(std::move(buf));
    }
    buffers_.clear();
  }
  for (size_t i = 0; i < buffers_to_write.size(); ++i) {
    output.Append(buffers_to_write[i]->GetData(), buffers_to_write[i]->Length());
  }
  output.Flush();
}

}  // namespace zrpc
