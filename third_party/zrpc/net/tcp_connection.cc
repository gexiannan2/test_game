#include "zrpc/net/tcp_connection.h"

#include "zrpc/base/logger.h"
#include "zrpc/net/event_loop.h"
#include "zrpc/net/socket.h"

namespace zrpc {
namespace {

bool SocketWouldBlock(int err) {
#ifdef _WIN32
  return err == WSAEWOULDBLOCK;
#else
  return err == EWOULDBLOCK || err == EAGAIN;
#endif
}

bool SocketIsResetError(int err) {
#ifdef _WIN32
  return err == WSAECONNRESET || err == WSAENOTCONN;
#else
  return err == EPIPE || err == ECONNRESET;
#endif
}

int LastSocketError() {
#ifdef _WIN32
  return static_cast<int>(WSAGetLastError());
#else
  return errno;
#endif
}

bool ExceedsLimit(size_t current, size_t incoming, size_t limit) {
  return current > limit || incoming > limit - current;
}

}  // namespace

TcpConnection::TcpConnection(EventLoop *loop, SocketHandle sockfd,
                             const std::any &context)
    : loop_(loop),
      sockfd_(sockfd),
      reading_(true),
      high_water_mark_(kDefaultHighWaterMark),
      max_input_buffer_size_(kDefaultMaxInputBufferSize),
      max_output_buffer_size_(kDefaultMaxOutputBufferSize),
      state_(kConnecting),
      channel_(new Channel(loop_, sockfd_)),
      context_(context) {
  channel_->SetReadCallback(std::bind(&TcpConnection::HandleRead, this));
  channel_->SetWriteCallback(std::bind(&TcpConnection::HandleWrite, this));
  channel_->SetCloseCallback(std::bind(&TcpConnection::HandleClose, this));
  channel_->SetErrorCallback(std::bind(&TcpConnection::HandleError, this));
}

TcpConnection::~TcpConnection() {
  assert(state_ == kDisconnected);
  socket::Close(sockfd_);
}

void TcpConnection::Shutdown() {
  if (state_ == kConnected) {
    SetState(kDisconnecting);
    loop_->RunInLoop(
        std::bind(&TcpConnection::ShutdownInLoop, shared_from_this()));
  }
}

void TcpConnection::SetState(StateE s) {
  state_.store(s, std::memory_order_release);
}

void TcpConnection::ForceClose() {
  if (state_ == kConnected || state_ == kDisconnecting) {
    SetState(kDisconnecting);
    loop_->QueueInLoop(
        std::bind(&TcpConnection::ForceCloseInLoop, shared_from_this()));
  }
}

void TcpConnection::ForceCloseInLoop() {
  loop_->AssertInLoopThread();
  if (state_ == kConnected || state_ == kDisconnecting) {
    // as if we received 0 byte in HandleRead()
    HandleClose();
  }
}

void TcpConnection::ShutdownInLoop() {
  loop_->AssertInLoopThread();
  if (!channel_->IsWriting()) {
    socket::Shutdown(sockfd_);
  }
}

void TcpConnection::ForceCloseDelay() { ForceClose(); }

void TcpConnection::ForceCloseWithDelay(double seconds) {
  if (state_ == kConnected || state_ == kDisconnecting) {
    SetState(kDisconnecting);
    loop_->RunAfter(
        seconds, false,
        std::bind(&TcpConnection::ForceCloseDelay, shared_from_this()));
  }
}

// ---------------------------------------------------------------------------
// handleRead — 对齐 muduo：n>0 交 message；n==0 close；n<0 只 handleError
// ---------------------------------------------------------------------------
void TcpConnection::HandleRead() {
  loop_->AssertInLoopThread();

  int save_errno = 0;
  ssize_t n = 0;

  if (read_hook_) {
    const size_t buffered =
        static_cast<size_t>(intput_buffer_.ReadableBytes());
    const size_t capacity =
        buffered < max_input_buffer_size_
            ? (max_input_buffer_size_ - buffered)
            : 0;
    if (capacity == 0) {
      // 缓冲已满：不关连接（muduo 无此逻辑），丢弃本次可读并告警
      LOG_WARN << "TcpConnection input buffer full, fd=" << sockfd_;
      return;
    }
    const size_t next_read_size = std::min<size_t>(65536, capacity);
    intput_buffer_.EnsureWritableBytes(static_cast<int32_t>(next_read_size));
    n = read_hook_(intput_buffer_.BeginWrite(),
                   std::min(next_read_size,
                            static_cast<size_t>(
                                intput_buffer_.WritableBytes())),
                   &save_errno);
    if (n > 0) {
      if (static_cast<size_t>(n) >
          static_cast<size_t>(intput_buffer_.WritableBytes())) {
        save_errno = EINVAL;
        n = -1;
      } else {
        intput_buffer_.HasWritten(static_cast<int32_t>(n));
      }
    } else if (n < 0 && save_errno == 0) {
      save_errno = LastSocketError();
    }
  } else {
    n = intput_buffer_.ReadFd(channel_->Getfd(), &save_errno);
  }

  if (n > 0) {
    if (message_callback_) {
      message_callback_(shared_from_this(), &intput_buffer_);
    }
  } else if (n == 0) {
    HandleClose();
  } else {
    if (SocketWouldBlock(save_errno)) {
      return;
    }
    errno = save_errno;
    LOG_SYSERR << "TcpConnection::HandleRead";
    HandleError();
    // muduo：读错误不在此 handleClose，等 POLLHUP / 对端再关
  }
}

// ---------------------------------------------------------------------------
// handleWrite — 对齐 muduo：写失败只打日志，不关连接
// ---------------------------------------------------------------------------
void TcpConnection::HandleWrite() {
  loop_->AssertInLoopThread();
  if (!channel_->IsWriting()) {
    LOG_TRACE << "Connection fd=" << sockfd_ << " is down, no more writing";
    return;
  }

  int save_errno = 0;
  ssize_t n = 0;
  if (write_hook_) {
    n = write_hook_(output_buffer_.Peek(),
                    static_cast<size_t>(output_buffer_.ReadableBytes()),
                    &save_errno);
    if (n < 0 && save_errno == 0) {
      save_errno = LastSocketError();
    }
  } else {
    n = socket::Write(channel_->Getfd(), output_buffer_.Peek(),
                      output_buffer_.ReadableBytes());
    if (n < 0) {
      save_errno = LastSocketError();
    }
  }

  if (n > 0) {
    if (static_cast<size_t>(n) >
        static_cast<size_t>(output_buffer_.ReadableBytes())) {
      LOG_SYSERR << "TcpConnection::HandleWrite invalid write length";
      return;
    }
    output_buffer_.Retrieve(static_cast<int32_t>(n));
    if (output_buffer_.ReadableBytes() == 0) {
      channel_->DisableWriting();
      if (write_complete_callback_) {
        loop_->QueueInLoop(
            std::bind(write_complete_callback_, shared_from_this()));
      }
      if (state_ == kDisconnecting) {
        ShutdownInLoop();
      }
    }
  } else {
    if (!SocketWouldBlock(save_errno)) {
      if (save_errno != 0) {
        errno = save_errno;
      }
      LOG_SYSERR << "TcpConnection::HandleWrite";
    }
  }
}

void TcpConnection::HandleClose() {
  loop_->AssertInLoopThread();
  if (state_ == kDisconnected) {
    return;
  }
  assert(state_ == kConnected || state_ == kDisconnecting);
  SetState(kDisconnected);
  channel_->DisableAll();

  std::shared_ptr<TcpConnection> guard_this(shared_from_this());
  if (connection_callback_) {
    connection_callback_(guard_this);
  }
  // must be the last line
  if (close_callback_) {
    close_callback_(guard_this);
  }
}

void TcpConnection::HandleError() {
  int err = socket::GetSocketError(channel_->Getfd());
  LOG_SYSERR << "TcpConnection::HandleError - SO_ERROR = " << err << " "
             << strerror(err);
}

void TcpConnection::StartRead() {
  loop_->RunInLoop(
      std::bind(&TcpConnection::StartReadInLoop, shared_from_this()));
}

void TcpConnection::StopRead() {
  loop_->RunInLoop(
      std::bind(&TcpConnection::StopReadInLoop, shared_from_this()));
}

void TcpConnection::StartReadInLoop() {
  loop_->AssertInLoopThread();
  if (!reading_ || !channel_->IsReading()) {
    channel_->EnableReading();
    reading_ = true;
  }
}

void TcpConnection::StopReadInLoop() {
  loop_->AssertInLoopThread();
  if (reading_ || channel_->IsReading()) {
    channel_->DisableReading();
    reading_ = false;
  }
}

void TcpConnection::Send(const void *message, int len) {
  if (len <= 0 || message == nullptr) {
    return;
  }
  Send(std::string_view(static_cast<const char *>(message),
                        static_cast<size_t>(len)));
}

void TcpConnection::Send(const std::string_view &message) {
  if (state_ == kConnected) {
    if (loop_->IsInLoopThread()) {
      SendInLoop(message.data(), message.size());
    } else {
      void (TcpConnection::*fp)(const std::string_view &message) =
          &TcpConnection::SendInLoop;
      loop_->RunInLoop(
          std::bind(fp, shared_from_this(), std::string(message)));
    }
  }
}

void TcpConnection::Send(Buffer *buf) {
  if (buf == nullptr) {
    return;
  }
  if (state_ == kConnected) {
    if (loop_->IsInLoopThread()) {
      SendInLoop(buf->Peek(), static_cast<size_t>(buf->ReadableBytes()));
      buf->RetrieveAll();
    } else {
      void (TcpConnection::*fp)(const std::string_view &message) =
          &TcpConnection::SendInLoop;
      loop_->RunInLoop(
          std::bind(fp, shared_from_this(), buf->RetrieveAllAsString()));
    }
  }
}

void TcpConnection::SendInLoop(const std::string_view &message) {
  SendInLoop(message.data(), message.size());
}

// ---------------------------------------------------------------------------
// sendInLoop — 对齐 muduo：faultError 不 handleClose，不追加缓冲
// ---------------------------------------------------------------------------
void TcpConnection::SendInLoop(const void *data, size_t len) {
  loop_->AssertInLoopThread();
  ssize_t nwrote = 0;
  size_t remaining = len;
  bool fault_error = false;

  if (state_ == kDisconnected) {
    LOG_WARN << "disconnected, give up writing";
    return;
  }
  if (data == nullptr || len == 0) {
    return;
  }

  if (!channel_->IsWriting() && output_buffer_.ReadableBytes() == 0) {
    nwrote = socket::Write(channel_->Getfd(), data,
                           static_cast<int32_t>(len));
    if (nwrote >= 0) {
      remaining = len - static_cast<size_t>(nwrote);
      if (remaining == 0 && write_complete_callback_) {
        loop_->QueueInLoop(
            std::bind(write_complete_callback_, shared_from_this()));
      }
    } else {
      nwrote = 0;
      const int err = LastSocketError();
      if (!SocketWouldBlock(err)) {
        errno = err;
        LOG_SYSERR << "TcpConnection::SendInLoop";
        if (SocketIsResetError(err)) {
          fault_error = true;
        }
      }
    }
  }

  if (!fault_error && remaining > 0) {
    size_t old_len = static_cast<size_t>(output_buffer_.ReadableBytes());
    if (ExceedsLimit(old_len, remaining, max_output_buffer_size_)) {
      LOG_WARN << "TcpConnection output buffer limit, give up writing, fd="
               << sockfd_;
      return;
    }
    if (old_len + remaining >= high_water_mark_ &&
        old_len < high_water_mark_ && high_water_mark_callback_) {
      loop_->QueueInLoop(std::bind(high_water_mark_callback_,
                                   shared_from_this(),
                                   old_len + remaining));
    }
    output_buffer_.Append(static_cast<const char *>(data) + nwrote, remaining);
    if (!channel_->IsWriting()) {
      channel_->EnableWriting();
    }
  }
}

void TcpConnection::SendPipe(const void *message, int len) {
  if (len <= 0 || message == nullptr) {
    return;
  }
  SendPipe(std::string_view(static_cast<const char *>(message),
                            static_cast<size_t>(len)));
}

void TcpConnection::SendPipe(const std::string_view &message) {
  if (state_ == kConnected) {
    if (loop_->IsInLoopThread()) {
      SendPipeInLoop(message.data(), message.size());
    } else {
      void (TcpConnection::*fp)(const std::string_view &message) =
          &TcpConnection::SendPipeInLoop;
      loop_->RunInLoop(
          std::bind(fp, shared_from_this(), std::string(message)));
    }
  }
}

void TcpConnection::SendPipe(Buffer *buf) {
  if (buf == nullptr) {
    return;
  }
  if (state_ == kConnected) {
    std::string data = buf->RetrieveAllAsString();
    if (loop_->IsInLoopThread()) {
      SendPipeInLoop(data);
    } else {
      void (TcpConnection::*fp)(const std::string_view &message) =
          &TcpConnection::SendPipeInLoop;
      loop_->RunInLoop(
          std::bind(fp, shared_from_this(), std::move(data)));
    }
  }
}

void TcpConnection::SendInLoopPipe() {
  if (state_ == kConnected) {
    if (loop_->IsInLoopThread()) {
      SendPipe();
    } else {
      void (TcpConnection::*fp)() = &TcpConnection::SendPipe;
      loop_->RunInLoop(std::bind(fp, shared_from_this()));
    }
  }
}

void TcpConnection::SendPipe() {
  loop_->AssertInLoopThread();
  if (!channel_->IsNoneEvent() && output_buffer_.ReadableBytes() > 0) {
    if (!channel_->IsWriting()) {
      channel_->EnableWriting();
    }
  }
}

void TcpConnection::SendPipeInLoop(const std::string_view &message) {
  SendPipeInLoop(message.data(), message.size());
}

// 只入队、不直接 write（管道发送路径）；超限放弃，不关连接
void TcpConnection::SendPipeInLoop(const void *message, size_t len) {
  loop_->AssertInLoopThread();
  if (state_ == kDisconnected || len == 0 || message == nullptr) {
    return;
  }
  const size_t old_len =
      static_cast<size_t>(output_buffer_.ReadableBytes());
  if (ExceedsLimit(old_len, len, max_output_buffer_size_)) {
    LOG_WARN << "TcpConnection output buffer limit, give up writing, fd="
             << sockfd_;
    return;
  }
  if (old_len + len >= high_water_mark_ && old_len < high_water_mark_ &&
      high_water_mark_callback_) {
    loop_->QueueInLoop(std::bind(high_water_mark_callback_, shared_from_this(),
                                 old_len + len));
  }
  output_buffer_.Append(message, len);
  if (!channel_->IsWriting()) {
    channel_->EnableWriting();
  }
}

void TcpConnection::BindSendPipeInLoop(TcpConnection *conn,
                                       const std::string_view &message) {
  conn->SendPipeInLoop(message.data(), message.size());
}

void TcpConnection::BindSendInLoop(TcpConnection *conn,
                                   const std::string_view &message) {
  conn->SendInLoop(message.data(), message.size());
}

void TcpConnection::ConnectEstablished() {
  loop_->AssertInLoopThread();
  if (state_ != kConnecting || channel_removed_) {
    return;
  }
  SetState(kConnected);
  socket::SetTcpNoDelay(sockfd_, true);
  channel_->SetTie(shared_from_this());
  channel_->EnableReading();
  if (connection_callback_) {
    connection_callback_(shared_from_this());
  }
}

void TcpConnection::ConnectDestroyed() {
  loop_->AssertInLoopThread();
  if (channel_removed_) {
    return;
  }
  // muduo：仅 kConnected 时再调一次 connectionCallback
  if (state_ == kConnected) {
    SetState(kDisconnected);
    channel_->DisableAll();
    if (connection_callback_) {
      connection_callback_(shared_from_this());
    }
  } else if (state_ == kConnecting || state_ == kDisconnecting) {
    SetState(kDisconnected);
    // Shutdown 后未走到 HandleClose 时仍可能带读事件；Remove 前必须 DisableAll
    channel_->DisableAll();
  }
  channel_->Remove();
  channel_removed_ = !channel_->IsAddedToLoop();
#ifdef _WIN32
  if (channel_removed_) {
    loop_->ReleaseSocketContext(sockfd_);
  }
#endif
}

}  // namespace zrpc
