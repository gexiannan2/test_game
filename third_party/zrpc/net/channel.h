#pragma once

#include <assert.h>
#include <string>
#include <memory>
#include <vector>
#include <functional>

#include "zrpc/net/socket.h"

namespace zrpc {
class EventLoop;

class Channel {
 public:
  typedef std::function<void()> EventCallback;

  Channel(EventLoop *loop, SocketHandle fd);

  ~Channel();

  void HandleEvent();

  void SetTie(const std::shared_ptr<void> &);

  void SetRevents(int32_t revt) { revents_ = revt; }

  void SetEvents(int32_t revt) { events_ = revt; }

  void SetIndex(int32_t idx) { index_ = idx; }

  void SetReadCallback(const EventCallback &&cb) {
    read_callback_ = std::move(cb);
  }

  void SetWriteCallback(const EventCallback &&cb) {
    write_callback_ = std::move(cb);
  }

  void SetCloseCallback(const EventCallback &&cb) {
    close_callback_ = std::move(cb);
  }

  void SetErrorCallback(const EventCallback &&cb) {
    error_callback_ = std::move(cb);
  }

  bool ReadEnabled() { return events_ & kReadEvent; }

  bool WriteEnabled() { return events_ & kWriteEvent; }

  bool IsNoneEvent() const { return events_ == kNoneEvent; }

  void EnableReading() {
    const int32_t old_events = events_;
    events_ |= kReadEvent;
    if (!Update()) {
      events_ = old_events;
    }
  }

  void DisableReading() {
    const int32_t old_events = events_;
    events_ &= ~kReadEvent;
    if (!Update()) {
      events_ = old_events;
    }
  }

  void EnableWriting() {
    const int32_t old_events = events_;
    events_ |= kWriteEvent;
    if (!Update()) {
      events_ = old_events;
    }
  }

  void DisableWriting() {
    const int32_t old_events = events_;
    events_ &= ~kWriteEvent;
    if (!Update()) {
      events_ = old_events;
    }
  }

  void DisableAll() {
    const int32_t old_events = events_;
    events_ = kNoneEvent;
    if (!Update()) {
      events_ = old_events;
    }
  }

  bool IsWriting() const { return events_ & kWriteEvent; }

  bool IsReading() const { return events_ & kReadEvent; }

  int32_t GetEvents() { return events_; }

  SocketHandle Getfd() const { return fd_; }

  void Remove();

  int32_t GetIndex() { return index_; }
  bool IsAddedToLoop() const { return added_to_loop_; }

  EventLoop *OwnerLoop() { return loop_; }

 private:
  Channel(const Channel &);

  void operator=(const Channel &);

  bool Update();

  void HandleEventWithGuard();

  EventCallback read_callback_;
  EventCallback write_callback_;
  EventCallback close_callback_;
  EventCallback error_callback_;

  static const int kNoneEvent;
  static const int kReadEvent;
  static const int kWriteEvent;

  EventLoop *loop_;
  SocketHandle fd_;
  int32_t events_;
  int32_t revents_;
  int32_t index_;
  bool tied_;
  bool event_handling_;
  bool added_to_loop_;
  bool log_hup_;
  std::weak_ptr<void> tie_;
};

}  // namespace zrpc
