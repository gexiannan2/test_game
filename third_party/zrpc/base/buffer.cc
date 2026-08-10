#include "zrpc/base/buffer.h"

namespace zrpc {
const char Buffer::kCRLF[] = "\r\n";
const int32_t Buffer::kCheapPrepend;
const int32_t Buffer::kInitialSize;

ssize_t Buffer::ReadFd(SocketHandle fd, int32_t *save_errno) {
  if (save_errno == nullptr) {
    throw std::invalid_argument(
        "Buffer::ReadFd requires an error output pointer");
  }
  char extrabuf[65536];
  IOV_TYPE vec[2];
  const int32_t writable = WritableBytes();
#ifdef _WIN32
  vec[0].buf = Begin() + writer_index_;
  vec[0].len = writable;
  vec[1].buf = extrabuf;
  vec[1].len = sizeof(extrabuf);
#else
  vec[0].iov_base = Begin() + writer_index_;
  vec[0].iov_len = writable;
  vec[1].iov_base = extrabuf;
  vec[1].iov_len = sizeof extrabuf;
#endif
  const int iovcnt =
      (writable < static_cast<int32_t>(sizeof(extrabuf))) ? 2 : 1;
  const ssize_t n = socket::Readv(fd, vec, iovcnt);
  if (n < 0) {
#ifdef _WIN32
    *save_errno = WSAGetLastError();
#else
    *save_errno = errno;
#endif
  } else if (n <= writable) {
    writer_index_ += static_cast<int32_t>(n);
  } else {
    writer_index_ = static_cast<int32_t>(buffer_.size());
    Append(extrabuf, static_cast<size_t>(n - writable));
  }
  return n;
}
}  // namespace zrpc
