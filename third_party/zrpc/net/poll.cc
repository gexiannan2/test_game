#ifdef __APPLE__

#include "zrpc/net/poll.h"

#include "zrpc/net/channel.h"
#include "zrpc/net/event_loop.h"
#include "zrpc/base/log.h"

namespace zrpc {
const int32_t kNew = -1;
const int32_t kAdded = 1;
const int32_t kDeleted = 2;

Poll::Poll(EventLoop *loop) : loop_(loop) {}

Poll::~Poll() {}

void Poll::EpollWait(ChannelList *active_channels, int ms_time) {
  const int64_t timer_ms = loop_->GetTimeOut();
  if (ms_time < 0 || timer_ms < ms_time) {
    ms_time = static_cast<int>(timer_ms);
  }

  int32_t num_events = ::poll(events_.data(), events_.size(), ms_time);
  int32_t saved_errno = errno;

  if (num_events > 0) {
    FillActiveChannels(num_events, active_channels);
  } else if (num_events == 0) {
  } else {
    if (saved_errno != EINTR) {
    }
  }
  loop_->HandlerTimerQueue();
}

bool Poll::HasChannel(Channel *channel) {
  loop_->AssertInLoopThread();
  auto it = channels_.find(channel->Getfd());
  return it != channels_.end() && it->second == channel;
}

bool Poll::UpdateChannel(Channel *channel) {
  loop_->AssertInLoopThread();
  if (channel->GetIndex() < 0) {
    assert(channels_.find(channel->Getfd()) == channels_.end());
    struct pollfd pfd;
    pfd.fd = channel->Getfd();
    pfd.events = static_cast<short>(channel->GetEvents());
    pfd.revents = 0;
    events_.push_back(pfd);
    int32_t idx = static_cast<int32_t>(events_.size()) - 1;
    channel->SetIndex(idx);
    channels_[pfd.fd] = channel;
  } else {
    assert(channels_.find(channel->Getfd()) != channels_.end());
    assert(channels_[channel->Getfd()] == channel);
    int32_t idx = channel->GetIndex();
    assert(0 <= idx && idx < static_cast<int32_t>(events_.size()));
    struct pollfd &pfd = events_[idx];
    assert(pfd.fd == channel->Getfd() || pfd.fd == -channel->Getfd() - 1);
    pfd.fd = channel->Getfd();
    pfd.events = static_cast<short>(channel->GetEvents());
    pfd.revents = 0;
    if (channel->IsNoneEvent()) {
      pfd.fd = -channel->Getfd() - 1;
    }
  }
  return true;
}

bool Poll::RemoveChannel(Channel *channel) {
  loop_->AssertInLoopThread();
  assert(channels_.find(channel->Getfd()) != channels_.end());
  assert(channels_[channel->Getfd()] == channel);
  assert(channel->IsNoneEvent());
  int32_t idx = channel->GetIndex();
  assert(0 <= idx && idx < static_cast<int32_t>(events_.size()));
  const struct pollfd &pfd = events_[idx];
  (void)pfd;
  assert(pfd.fd == -channel->Getfd() - 1 && pfd.events == channel->GetEvents());
  size_t n = channels_.erase(channel->Getfd());
  assert(n == 1);
  (void)n;
  if (idx == events_.size() - 1) {
    events_.pop_back();
  } else {
    int32_t channel_at_end = events_.back().fd;
    iter_swap(events_.begin() + idx, events_.end() - 1);
    if (channel_at_end < 0) {
      channel_at_end = -channel_at_end - 1;
    }

    channels_[channel_at_end]->SetIndex(idx);
    events_.pop_back();
  }
  return true;
}

void Poll::FillActiveChannels(int32_t num_events,
                              ChannelList *active_channels) const {
  for (auto it = events_.begin(); it != events_.end() && num_events > 0; ++it) {
    if ((*it).revents > 0) {
      --num_events;
      auto iter = channels_.find((*it).fd);
      assert(iter != channels_.end());
      auto channel = iter->second;
      assert(channel->Getfd() == (*it).fd);
      channel->SetRevents((*it).revents);
      active_channels->push_back(channel);
    }
  }
}

}  // namespace zrpc
#endif
