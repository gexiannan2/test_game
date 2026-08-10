
#pragma once
#include <google/protobuf/io/zero_copy_stream.h>
#include "zrpc/base/logger.h"
#include "zrpc/base/buffer.h"

namespace zrpc {
class BufferInputStream : public google::protobuf::io::ZeroCopyInputStream {
 public:
  BufferInputStream(Buffer* buf, int32_t original_size)
      : buffer_(buf), original_size_(original_size), byte_count_(0) {}

  virtual bool Next(const void** data, int* size) {
    const int32_t remaining = original_size_ - byte_count_;
    if (remaining <= 0) {
      return false;
    }

    *data = buffer_->Peek() + byte_count_;
    *size = remaining;
    byte_count_ += remaining;
    return true;
  }

  virtual bool Skip(int count) {
    if (count < 0) {
      return false;
    }
    const int32_t remaining = original_size_ - byte_count_;
    if (count > remaining) {
      return false;
    }
    byte_count_ += count;
    return true;
  }

  virtual void BackUp(int count) {
    if (count <= 0) {
      return;
    }
    byte_count_ -= count;
    if (byte_count_ < 0) {
      byte_count_ = 0;
    }
  }

  virtual int64_t ByteCount() const {
    return byte_count_;
  }

 private:
  Buffer* buffer_;
  int32_t original_size_;
  int32_t byte_count_;
};

class BufferOutputStream : public google::protobuf::io::ZeroCopyOutputStream {
 public:
  BufferOutputStream(Buffer* buf)
      : buffer_(buf), original_size_(buffer_->ReadableBytes()) {}

  virtual bool Next(void** data, int* size) {
    buffer_->EnsureWritableBytes(4096);
    *data = buffer_->BeginWrite();
    *size = static_cast<int>(buffer_->WritableBytes());
    buffer_->HasWritten(*size);
    return true;
  }

  virtual void BackUp(int count) { buffer_->UnWrite(count); }

  virtual int64_t ByteCount() const {
    return buffer_->ReadableBytes() - original_size_;
  }

 private:
  Buffer* buffer_;
  size_t original_size_;
};
}  // namespace zrpc
