#pragma once

#ifdef __linux__
#include <assert.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "zrpc/base/log.h"

namespace zrpc {
class Channel;

class EventLoop;

class Epoll {
 public:
  typedef std::vector<struct epoll_event> EventList;
  typedef std::vector<Channel *> ChannelList;
  typedef std::unordered_map<int32_t, Channel *> ChannelMap;

  Epoll(EventLoop *loop);

  ~Epoll();

  void EpollWait(ChannelList *active_channels, int32_t ms_time = 100);
  bool HasChannel(Channel *channel);
  bool UpdateChannel(Channel *channel);
  bool RemoveChannel(Channel *channel);
  bool Update(int32_t operation, Channel *channel);
  void FillActiveChannels(int32_t num_events,
                          ChannelList *active_channels) const;

 private:
  Epoll(const Epoll &);

  void operator=(const Epoll &);

  static const int kInitEventListSize = 16;
  ChannelMap channels;
  EventList events_;
  EventLoop *loop_;
  int32_t epollfd_;
};
}  // namespace zrpc
#endif
