#pragma once

#include <atomic>
#include <any>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "zrpc/base/buffer.h"
#include "zrpc/net/callback.h"
#include "zrpc/net/channel.h"

namespace zrpc {
struct Message {
  Message(uint16_t msg_size, uint16_t msg_type)
      : size(msg_size), type(msg_type) {}

  Message() : size(0), type(0) {}

  uint16_t size;
  uint16_t type;
};

class EventLoop;

class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
 public:
  enum StateE { kDisconnected, kConnecting, kConnected, kDisconnecting };

  static constexpr size_t kDefaultMaxInputBufferSize = 16U * 1024U * 1024U;
  static constexpr size_t kDefaultMaxOutputBufferSize = 64U * 1024U * 1024U;
  static constexpr size_t kDefaultHighWaterMark = 8U * 1024U * 1024U;

  TcpConnection(EventLoop *loop, SocketHandle sockfd, const std::any &);

  ~TcpConnection();

  EventLoop *GetLoop() { return loop_; }

  SocketHandle GetSockfd() const { return sockfd_; }

  void SetState(StateE s);

  void SetConnectionCallback(ConnectionCallback cb) {
    connection_callback_ = std::move(cb);
  }

  void SetMessageCallback(MessageCallback cb) {
    message_callback_ = std::move(cb);
  }

 //void SetMessage1Callback(const Message1Callback &&cb) {
 //   message1_callback_ = std::move(cb);
 // }

  void SetWriteCompleteCallback(WriteCompleteCallback cb) {
    write_complete_callback_ = std::move(cb);
  }

  void SetHighWaterMarkCallback(HighWaterMarkCallback cb,
                                size_t high_water_mark) {
    high_water_mark_callback_ = std::move(cb);
    high_water_mark_ = high_water_mark;
  }

  bool SetBufferLimits(size_t max_input_bytes, size_t max_output_bytes) {
    if (max_input_bytes == 0 || max_output_bytes == 0) {
      return false;
    }
    max_input_buffer_size_ = max_input_bytes;
    max_output_buffer_size_ = max_output_bytes;
    if (high_water_mark_ > max_output_buffer_size_) {
      high_water_mark_ = max_output_buffer_size_;
    }
    return true;
  }

  void SetCloseCallback(const CloseCallback &cb) { close_callback_ = cb; }

  void SendInLoop(const void *message, size_t len);
  void SendInLoop(const std::string_view &message);
  void SendPipeInLoop(const void *message, size_t len);
  void SendPipeInLoop(const std::string_view &message);

  //void AdjustRevBuffer(uint16_t new_size);
  //ssize_t Read(int32_t fd, int32_t *save_errno);
  //bool ParseMsgBuffer();

  static void BindSendInLoop(TcpConnection *conn,
                             const std::string_view &message);

  static void BindSendPipeInLoop(TcpConnection *conn,
                                 const std::string_view &message);

  void Send(const void *message, int len);
  void SendPipe(const void *message, int len);
  void SendInLoopPipe();
  void SendPipe();
  void SendPipe(const std::string_view &message);
  void SendPipe(Buffer *message);
  void Send(Buffer *message);
  void Send(const std::string_view &message);

  bool Disconnected() const {
    return state_.load(std::memory_order_acquire) == kDisconnected;
  }
  bool Connected() {
    return state_.load(std::memory_order_acquire) == kConnected;
  }
  void ForceCloseInLoop();
  void ConnectEstablished();
  void ForceCloseWithDelay(double seconds);
  void ForceCloseDelay();
  void ConnectDestroyed();
  void Shutdown();
  void ShutdownInLoop();
  void ForceClose();

  void HandleRead();
  void HandleWrite();
  void HandleClose();
  void HandleError();

  void StartReadInLoop();
  void StopReadInLoop();
  void StartRead();
  void StopRead();

  std::any *GetMutableContext() { return &context_; }
  const std::any &GetContext() const { return context_; }
  void ResetContext() { context_.reset(); }
  std::any *GetMutableContext1() { return &context1_; }
  const std::any &GetContext1() const { return context1_; }

  void ResetContext1() { context1_.reset(); }
  void SetContext(const std::any &context) { context_ = context; }
  void SetContext1(const std::any &context) { context1_ = context; }

  Buffer *OutputBuffer() { return &output_buffer_; }
  Buffer *IntputBuffer() { return &intput_buffer_; }

  using ReadHook = std::function<ssize_t(char *buf, size_t len, int *save_errno)>;
  using WriteHook =
      std::function<ssize_t(const char *buf, size_t len, int *save_errno)>;

  void SetReadHook(ReadHook hook) { read_hook_ = std::move(hook); }
  void SetWriteHook(WriteHook hook) { write_hook_ = std::move(hook); }
  void ClearIoHooks() {
    read_hook_ = nullptr;
    write_hook_ = nullptr;
  }

 private:
  TcpConnection(const TcpConnection &);

  void operator=(const TcpConnection &);

  EventLoop *loop_;
  SocketHandle sockfd_;
  bool reading_;
  bool channel_removed_{false};

  Buffer output_buffer_;
  Buffer intput_buffer_;
  ConnectionCallback connection_callback_;
  MessageCallback message_callback_;
  // Message1Callback message1_callback_;
  WriteCompleteCallback write_complete_callback_;
  HighWaterMarkCallback high_water_mark_callback_;
  CloseCallback close_callback_;

  size_t high_water_mark_;
  size_t max_input_buffer_size_;
  size_t max_output_buffer_size_;
  std::atomic<StateE> state_;
  std::shared_ptr<Channel> channel_;
  std::any context_;
  std::any context1_;
  ReadHook read_hook_;
  WriteHook write_hook_;
  //union {
  //  Message *header;
  //  char *buffer;
  //} rmsg_;

  //size_t rcap_;
  //size_t rpos_;
};
}  // namespace zrpc
