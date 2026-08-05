#pragma once

#ifdef __APPLE__
#include <libkern/OSByteOrder.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/event.h>
#include <sys/poll.h>
#include <sys/un.h>
#include <unordered_map>
#include <vector>

namespace zrpc {
class Channel;

class EventLoop;

class Poll {
 public:
  typedef std::vector<pollfd> EventList;
  typedef std::vector<Channel *> ChannelList;
  typedef std::unordered_map<int32_t, Channel *> ChannelMap;

  Poll(EventLoop *loop);

  ~Poll();

  void EpollWait(ChannelList *active_channels, int ms_time = 100);
  bool HasChannel(Channel *channel);
  bool UpdateChannel(Channel *channel);
  bool RemoveChannel(Channel *channel);
  void FillActiveChannels(int32_t numEvents,
                          ChannelList *active_channels) const;

 private:
  Poll(const Poll &);

  void operator=(const Poll &);

  ChannelMap channels_;
  EventList events_;
  EventLoop *loop_;
};

}  // namespace zrpc
#endif
