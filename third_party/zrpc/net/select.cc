#ifdef _WIN32

#include "zrpc/net/select.h"

#include "zrpc/base/log.h"
#include "zrpc/net/channel.h"
#include "zrpc/net/event_loop.h"
#include <algorithm>

namespace zrpc {
Select::Select(EventLoop *loop) : loop_(loop) {}

Select::~Select() {}

void Select::EpollWait(ChannelList *active_channels, int32_t ms_time) {
  FD_ZERO(&rfds_);
  FD_ZERO(&wfds_);
  FD_ZERO(&efds_);

  for (auto &it : events_) {
    if (!socket::IsValid(it.fd) || it.events == 0) {
      continue;
    }
    if ((it.events) & (POLLIN | POLLPRI)) {
      FD_SET(it.fd, &rfds_);
    }

    if ((it.events) & (POLLOUT)) {
      FD_SET(it.fd, &wfds_);
    }

    FD_SET(it.fd, &efds_);
  }

  auto timer_queue = loop_->GetTimerQueue();
  const int64_t timer_ms = timer_queue->GetTimeout();
  if (ms_time < 0 || timer_ms < ms_time) {
    ms_time = static_cast<int32_t>(timer_ms);
  }

  timeval timeout;
  timeout.tv_sec = ms_time / 1000;
  timeout.tv_usec = static_cast<int>(ms_time % 1000) * 1000;

  int num_events = ::select(0, &rfds_, &wfds_, &efds_, &timeout);
  int save_errno = WSAGetLastError();

  if (num_events > 0) {
    FillActiveChannels(num_events, active_channels);
  } else if (num_events == 0) {
  } else {
    if (save_errno != WSAEINTR) {
      errno = save_errno;
    }
  }
  loop_->HandlerTimerQueue();
}

bool Select::HasChannel(Channel *channel) {
  loop_->AssertInLoopThread();
  auto it = channels_.find(channel->Getfd());
  return it != channels_.end() && it->second == channel;
}

bool Select::UpdateChannel(Channel *channel) {
  loop_->AssertInLoopThread();
  if (!socket::IsValid(channel->Getfd())) {
    LOG_WARN << "Select rejected an invalid socket";
    return false;
  }
  if (channel->GetIndex() < 0) {
    if (events_.size() >= FD_SETSIZE) {
      LOG_WARN << "Select FD_SETSIZE limit reached";
      return false;
    }
    assert(channels_.find(channel->Getfd()) == channels_.end());
    struct pollfd it;
    it.fd = channel->Getfd();
    it.events = static_cast<short>(channel->GetEvents());
    it.revents = 0;
    events_.push_back(it);
    int32_t idx = static_cast<int32_t>(events_.size()) - 1;
    channel->SetIndex(idx);
    channels_[it.fd] = channel;
  } else {
    assert(channels_.find(channel->Getfd()) != channels_.end());
    assert(channels_[channel->Getfd()] == channel);
    int32_t idx = channel->GetIndex();
    assert(0 <= idx && idx < static_cast<int32_t>(events_.size()));
    struct pollfd &it = events_[idx];
    assert(it.fd == channel->Getfd());
    it.fd = channel->Getfd();
    it.events = static_cast<short>(channel->GetEvents());
    it.revents = 0;
    if (channel->IsNoneEvent()) {
      it.events = 0;
    }
  }
  return true;
}

bool Select::RemoveChannel(Channel *channel) {
  loop_->AssertInLoopThread();
  assert(channels_.find(channel->Getfd()) != channels_.end());
  assert(channels_[channel->Getfd()] == channel);
  assert(channel->IsNoneEvent());
  int32_t idx = channel->GetIndex();
  assert(0 <= idx && idx < static_cast<int32_t>(events_.size()));
  const struct pollfd &it = events_[idx];
  (void)it;
  assert(it.fd == channel->Getfd() && it.events == 0);
  size_t n = channels_.erase(channel->Getfd());
  assert(n == 1);
  (void)n;
  if (idx == events_.size() - 1) {
    events_.pop_back();
  } else {
    SocketHandle channel_at_end = events_.back().fd;
    iter_swap(events_.begin() + idx, events_.end() - 1);
    channels_[channel_at_end]->SetIndex(idx);
    events_.pop_back();
  }
  return true;
}

void Select::FillActiveChannels(int32_t num_events,
                                ChannelList *active_channels) const {
  for (auto it = events_.begin(); it != events_.end() && num_events > 0; ++it) {
    if (!socket::IsValid(it->fd) || it->events == 0) {
      continue;
    }
    int revents = 0;
    if (FD_ISSET(it->fd, &rfds_)) {
      revents |= POLLIN;
    }

    if (FD_ISSET(it->fd, &wfds_)) {
      revents |= POLLOUT;
    }

    if (FD_ISSET(it->fd, &efds_)) {
      revents |= POLLERR;
    }

    if (revents > 0) {
      --num_events;
      auto iter = channels_.find(it->fd);
      assert(iter != channels_.end());
      Channel *channel = iter->second;
      assert(channel->Getfd() == it->fd);
      channel->SetRevents(revents);
      active_channels->push_back(channel);
    }
  }
}

}  // namespace zrpc
#endif
