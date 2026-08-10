#include "zrpc/net/channel.h"

#include "zrpc/net/event_loop.h"

namespace zrpc {
namespace {
class EventHandlingGuard {
 public:
  explicit EventHandlingGuard(bool* handling) : handling_(handling) {
    *handling_ = true;
  }

  ~EventHandlingGuard() { *handling_ = false; }

 private:
  bool* handling_;
};
}  // namespace

const int Channel::kNoneEvent = 0;
const int Channel::kReadEvent = POLLIN | POLLPRI;
const int Channel::kWriteEvent = POLLOUT;

Channel::Channel(EventLoop* loop, SocketHandle fd)
    : loop_(loop),
      fd_(fd),
      events_(0),
      revents_(0),
      index_(-1),
      tied_(false),
      event_handling_(false),
      added_to_loop_(false),
      log_hup_(false) {}

Channel::~Channel() {
  assert(!event_handling_);
  assert(!added_to_loop_);

  if (loop_->IsInLoopThread()) {
    assert(!loop_->HasChannel(this));
  }
}

void Channel::Remove() {
  assert(IsNoneEvent());
  if (!added_to_loop_) {
    return;
  }
  if (loop_->RemoveChannel(this)) {
    added_to_loop_ = false;
  }
}

bool Channel::Update() {
  if (loop_->UpdateChannel(this)) {
    added_to_loop_ = true;
    return true;
  }
  return false;
}

void Channel::HandleEventWithGuard() {
  EventHandlingGuard handling_guard(&event_handling_);

  if ((revents_ & POLLHUP) && !(revents_ & POLLIN)) {
    if (log_hup_) {
    }

    if (close_callback_) {
      close_callback_();
    }
  }

  if (revents_ & POLLNVAL) {
  }

  if (revents_ & (POLLERR | POLLNVAL)) {
    if (error_callback_) {
      error_callback_();
    }
  }

  #ifndef POLLRDHUP
   const int POLLRDHUP = 0;
  #endif

  if (revents_ & (POLLIN | POLLPRI | POLLRDHUP)) {
    if (read_callback_) {
      read_callback_();
    }
  }

  if (revents_ & POLLOUT) {
    if (write_callback_) {
      write_callback_();
    }
  }
}

void Channel::HandleEvent() {
  std::shared_ptr<void> guard;
  if (tied_) {
    guard = tie_.lock();
    if (guard) {
      HandleEventWithGuard();
    }
  } else {
    HandleEventWithGuard();
  }
}

void Channel::SetTie(const std::shared_ptr<void>& obj) {
  tie_ = obj;
  tied_ = true;
}
}  // namespace zrpc
