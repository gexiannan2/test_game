#ifdef __linux__

#include "zrpc/net/epoll.h"

#include "zrpc/net/channel.h"
#include "zrpc/net/event_loop.h"
#include "zrpc/base/logger.h"

namespace zrpc {
const int32_t kNew = -1;
const int32_t kAdded = 1;
const int32_t kDeleted = 2;

Epoll::Epoll(EventLoop *loop)
    : events_(kInitEventListSize),
      loop_(loop),
      epollfd_(::epoll_create1(EPOLL_CLOEXEC)) {
  if (epollfd_ < 0) {
    LOG_FATAL << "Epoll::Epoll";
  }
}

Epoll::~Epoll() { socket::Close(epollfd_); }

void Epoll::EpollWait(ChannelList *active_channels, int32_t ms_time) {
  int32_t num_events =
      ::epoll_wait(epollfd_, events_.data(), events_.size(), ms_time);
  int32_t saved_errno = errno;

  if (num_events > 0) {
    FillActiveChannels(num_events, active_channels);
    if (static_cast<size_t>(num_events) == events_.size()) {
      events_.resize(events_.size() * 2);
    }
  } else if (num_events == 0) {
  } else {
    if (saved_errno != EINTR) {
    }
  }
}

bool Epoll::HasChannel(Channel *channel) {
  loop_->AssertInLoopThread();
  auto it = channels.find(channel->Getfd());
  return it != channels.end() && it->second == channel;
}

bool Epoll::UpdateChannel(Channel *channel) {
  loop_->AssertInLoopThread();
  const int32_t index = channel->GetIndex();
  if (index == kNew || index == kDeleted) {
    const SocketHandle fd = channel->Getfd();
    if (index == kNew) {
      assert(channels.find(fd) == channels.end());
    } else {
      assert(channels.find(fd) != channels.end());
      assert(channels[fd] == channel);
    }
    if (!Update(EPOLL_CTL_ADD, channel)) {
      return false;
    }
    if (index == kNew) {
      channels[fd] = channel;
    }
    channel->SetIndex(kAdded);
  } else {
    const SocketHandle fd = channel->Getfd();
    (void)fd;
    assert(channels.find(fd) != channels.end());
    assert(channels[fd] == channel);
    assert(index == kAdded);
    if (channel->IsNoneEvent()) {
      if (!Update(EPOLL_CTL_DEL, channel)) {
        return false;
      }
      channel->SetIndex(kDeleted);
    } else {
      if (!Update(EPOLL_CTL_MOD, channel)) {
        return false;
      }
    }
  }
  return true;
}

bool Epoll::RemoveChannel(Channel *channel) {
  loop_->AssertInLoopThread();
  const SocketHandle fd = channel->Getfd();
  int32_t index = channel->GetIndex();
  assert(channels.find(fd) != channels.end());
  assert(channels[fd] == channel);
  assert(channel->IsNoneEvent());
  assert(index == kAdded || index == kDeleted);
  if (index == kAdded) {
    if (!Update(EPOLL_CTL_DEL, channel)) {
      return false;
    }
  }

  size_t n = channels.erase(fd);
  (void)n;
  assert(n == 1);
  channel->SetIndex(kNew);
  return true;
}

bool Epoll::Update(int32_t operation, Channel *channel) {
  struct epoll_event event;
  bzero(&event, sizeof event);
  event.events = channel->GetEvents();
  event.data.ptr = channel;
  const SocketHandle fd = channel->Getfd();
  if (::epoll_ctl(epollfd_, operation, fd, &event) < 0) {
    const int saved_errno = errno;
    LOG_WARN << "epoll_ctl failed, operation=" << operation
             << " fd=" << fd << " error=" << strerror(saved_errno);
    return false;
  }
  return true;
}

void Epoll::FillActiveChannels(int32_t num_events,
                               ChannelList *active_channels) const {
  for (int32_t i = 0; i < num_events; ++i) {
    Channel *channel = static_cast<Channel *>(events_[i].data.ptr);
    const SocketHandle fd = channel->Getfd();
    auto it = channels.find(fd);
    assert(it != channels.end());
    assert(it->second == channel);
    channel->SetRevents(events_[i].events);
    active_channels->push_back(channel);
  }
}

}  // namespace zrpc
#endif
