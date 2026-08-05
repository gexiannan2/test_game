#pragma once

#include "zrpc/net/socket.h"
#include <vector>
#include <algorithm>
#include <assert.h>
#include <string_view>
#include <string>
#include <errno.h>
#include <limits>
#include <stdexcept>

namespace zrpc {
class Buffer {
 public:
  static const int32_t kCheapPrepend = 8;
  static const int32_t kInitialSize = 1024;

  explicit Buffer(int32_t initialsize = kInitialSize)
      : buffer_(kCheapPrepend + CheckInitialSize(initialsize)),
        reader_index_(kCheapPrepend),
        writer_index_(kCheapPrepend) {
    assert(ReadableBytes() == 0);
    assert(WritableBytes() == initialsize);
    assert(PrependableBytes() == kCheapPrepend);
  }

  void Swap(Buffer &rhs) {
    buffer_.swap(rhs.buffer_);
    std::swap(reader_index_, rhs.reader_index_);
    std::swap(writer_index_, rhs.writer_index_);
  }

  int32_t ReadableBytes() const { return writer_index_ - reader_index_; }
  int32_t WritableBytes() const {
    return static_cast<int32_t>(buffer_.size()) - writer_index_;
  }
  int32_t GetWriterIndex() const { return writer_index_; }
  int32_t PrependableBytes() const { return reader_index_; }

  const char *Peek() const { return Begin() + reader_index_; }
  const char *Start() { return Begin() + kCheapPrepend; }
  char *Data() { return Begin() + reader_index_; }

  const char *FindCRLF() const {
    const char *crlf = std::search(Peek(), BeginWrite(), kCRLF, kCRLF + 2);
    return crlf == BeginWrite() ? nullptr : crlf;
  }

  const char *FindCRLF(const char *start) const {
    assert(Peek() <= start);
    assert(start <= BeginWrite());
    const char *crlf = std::search(start, BeginWrite(), kCRLF, kCRLF + 2);
    return crlf == BeginWrite() ? nullptr : crlf;
  }

  const char *FindEOL() const {
    const void *eol = memchr(Peek(), '\n', ReadableBytes());
    return static_cast<const char *>(eol);
  }

  const char *FindEOL(const char *start) const {
    assert(Peek() <= start);
    assert(start <= BeginWrite());
    const void *eol = memchr(start, '\n', BeginWrite() - start);
    return static_cast<const char *>(eol);
  }

  void Retrieve(int32_t len) {
    CheckLength(len);
    if (len > ReadableBytes()) {
      throw std::out_of_range("Buffer retrieve exceeds readable data");
    }
    assert(len <= ReadableBytes());
    if (len < ReadableBytes()) {
      reader_index_ += len;
    } else {
      RetrieveAll();
    }
  }

  void RetrieveUntil(const char *end) {
    assert(Peek() <= end);
    assert(end <= BeginWrite());
    Retrieve(static_cast<int32_t>(end - Peek()));
  }

  void RetrieveInt64() { Retrieve(sizeof(int64_t)); }
  void RetrieveInt32() { Retrieve(sizeof(int32_t)); }
  void RetrieveInt16() { Retrieve(sizeof(int16_t)); }
  void RetrieveInt8() { Retrieve(sizeof(int8_t)); }

  void RetrieveAll() {
    reader_index_ = kCheapPrepend;
    writer_index_ = kCheapPrepend;
  }

  std::string RetrieveAsString(int32_t len) {
    CheckLength(len);
    if (len > ReadableBytes()) {
      throw std::out_of_range("Buffer retrieve exceeds readable data");
    }
    assert(len <= ReadableBytes());
    std::string result(Peek(), len);
    Retrieve(len);
    return result;
  } 

  std::string RetrieveAllAsString() {
    return RetrieveAsString(ReadableBytes());
  }

  void Append(const char *data) {
    if (data == nullptr) {
      throw std::invalid_argument("Buffer append data must not be null");
    }
    const int32_t len = CheckedSize(strlen(data));
    EnsureWritableBytes(len);
    std::copy(data, data + len, BeginWrite());
    HasWritten(len);
  }

  void Append(const char *data, size_t len) {
    if (len == 0) {
      return;
    }
    if (data == nullptr) {
      throw std::invalid_argument("Buffer append data must not be null");
    }
    const int32_t checked_len = CheckedSize(len);
    EnsureWritableBytes(checked_len);
    std::copy(data, data + checked_len, BeginWrite());
    HasWritten(checked_len);
  }

  void Append(const std::string_view &str) { Append(str.data(), str.size()); }

  void Append(const void *data, size_t len) {
    Append(static_cast<const char *>(data), len);
  }

  void AppendInt32(int32_t x) {
    int32_t be32 = socket::HostToNetwork32(x);
    Append(&be32, sizeof be32);
  }

  void AppendInt64(int64_t x) {
    int64_t be64 = socket::HostToNetwork64(x);
    Append(&be64, sizeof be64);
  }

  void AppendInt16(int16_t x) {
    int16_t be16 = socket::HostToNetwork16(x);
    Append(&be16, sizeof be16);
  }

  void AppendInt8(int8_t x) { Append(&x, sizeof x); }

  void PrependInt64(int64_t x) {
    int64_t be64 = socket::HostToNetwork64(x);
    Prepend(&be64, sizeof be64);
  }

  void PrependInt32(int32_t x) {
    int32_t be32 = socket::HostToNetwork32(x);
    Prepend(&be32, sizeof be32);
  }

  void PrependInt16(int16_t x) {
    int16_t be16 = socket::HostToNetwork16(x);
    Prepend(&be16, sizeof be16);
  }

  void PrependInt8(int8_t x) {
    int8_t be8 = x;
    Prepend(&be8, sizeof be8);
  }

  void Prepend(const void *data, int32_t len) {
    CheckLength(len);
    if (len == 0) {
      return;
    }
    if (data == nullptr) {
      throw std::invalid_argument("Buffer prepend data must not be null");
    }
    if (len > PrependableBytes()) {
      throw std::out_of_range("Buffer prepend exceeds prependable space");
    }
    assert(len <= PrependableBytes());
    reader_index_ -= len;
    const char *d = static_cast<const char *>(data);
    std::copy(d, d + len, Begin() + reader_index_);
  }

  void EnsureWritableBytes(int32_t len) {
    CheckLength(len);
    if (len > std::numeric_limits<int32_t>::max() - writer_index_) {
      throw std::length_error("Buffer capacity exceeds int32_t");
    }
    if (WritableBytes() < len) {
      MakeSpace(len);
    }
    assert(WritableBytes() >= len);
  }

  char *BeginWrite() { return Begin() + writer_index_; }

  const char *BeginWrite() const { return Begin() + writer_index_; }

  void HasWritten(int32_t len) {
    CheckLength(len);
    if (len > WritableBytes()) {
      throw std::out_of_range("Buffer write exceeds writable space");
    }
    assert(len <= WritableBytes());
    writer_index_ += len;
  }

  void UnWrite(int32_t len) {
    CheckLength(len);
    if (len > ReadableBytes()) {
      throw std::out_of_range("Buffer unwrite exceeds readable data");
    }
    assert(len <= ReadableBytes());
    writer_index_ -= len;
  }

  int64_t ReadInt64() {
    int64_t result = PeekInt64();
    RetrieveInt64();
    return result;
  }

  int32_t ReadInt32() {
    int32_t result = PeekInt32();
    RetrieveInt32();
    return result;
  }

  int16_t ReadInt16() {
    int16_t result = PeekInt16();
    RetrieveInt16();
    return result;
  }

  int8_t ReadInt8() {
    int8_t result = PeekInt8();
    RetrieveInt8();
    return result;
  }

  int64_t PeekInt64() const {
    if (ReadableBytes() < static_cast<int32_t>(sizeof(int64_t))) {
      throw std::out_of_range("Buffer does not contain int64_t");
    }
    assert(ReadableBytes() >= static_cast<int32_t>(sizeof(int64_t)));
    int64_t be64 = 0;
    ::memcpy(&be64, Peek(), sizeof be64);
    return socket::NetworkToHost64(be64);
  }

  int32_t PeekInt32() const {
    if (ReadableBytes() < static_cast<int32_t>(sizeof(int32_t))) {
      throw std::out_of_range("Buffer does not contain int32_t");
    }
    assert(ReadableBytes() >= static_cast<int32_t>(sizeof(int32_t)));
    int32_t be32 = 0;
    ::memcpy(&be32, Peek(), sizeof be32);
    return socket::NetworkToHost32(be32);
  }

  int16_t PeekInt16() const {
    if (ReadableBytes() < static_cast<int32_t>(sizeof(int16_t))) {
      throw std::out_of_range("Buffer does not contain int16_t");
    }
    assert(ReadableBytes() >= static_cast<int32_t>(sizeof(int16_t)));
    int16_t be16 = 0;
    ::memcpy(&be16, Peek(), sizeof be16);
    return socket::NetworkToHost16(be16);
  }

  int8_t PeekInt8() const {
    if (ReadableBytes() < static_cast<int32_t>(sizeof(int8_t))) {
      throw std::out_of_range("Buffer does not contain int8_t");
    }
    assert(ReadableBytes() >= static_cast<int32_t>(sizeof(int8_t)));
    int8_t be8 = *Peek();
    return be8;
  }

  std::string_view ToStringView() const {
    return std::string_view(Peek(), static_cast<size_t>(ReadableBytes()));
  }

  void Shrink(int32_t reserve) {
    CheckLength(reserve);
    Buffer other;
    other.EnsureWritableBytes(ReadableBytes() + reserve);
    other.Append(ToStringView());
    Swap(other);
  }

  size_t InternalCapacity() const { return buffer_.capacity(); }

  ssize_t ReadFd(SocketHandle fd, int32_t *saved_errno);

 private:
  Buffer(const Buffer &);

  void operator=(const Buffer &);

  static int32_t CheckInitialSize(int32_t initial_size) {
    if (initial_size < 0 ||
        initial_size > std::numeric_limits<int32_t>::max() - kCheapPrepend) {
      throw std::invalid_argument("Buffer initial size is out of range");
    }
    return initial_size;
  }

  static void CheckLength(int32_t len) {
    if (len < 0) {
      throw std::invalid_argument("Buffer length must not be negative");
    }
  }

  static int32_t CheckedSize(size_t len) {
    if (len > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
      throw std::length_error("Buffer length exceeds int32_t");
    }
    return static_cast<int32_t>(len);
  }

  char *Begin() { return &*buffer_.begin(); }

  const char *Begin() const { return &*buffer_.begin(); }

  void MakeSpace(int32_t len) {
    if (WritableBytes() + PrependableBytes() < len + kCheapPrepend) {
      buffer_.resize(writer_index_ + len);
    } else {
      assert(kCheapPrepend < reader_index_);
      int32_t readable = ReadableBytes();
      std::copy(Begin() + reader_index_, Begin() + writer_index_,
                Begin() + kCheapPrepend);
      reader_index_ = kCheapPrepend;
      writer_index_ = reader_index_ + readable;
      assert(readable == ReadableBytes());
    }
  }

 private:
  std::vector<char> buffer_;
  int32_t reader_index_;
  int32_t writer_index_;

  static const char kCRLF[];
};
}  // namespace zrpc
