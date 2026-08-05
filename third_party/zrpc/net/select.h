#pragma once

#ifdef _WIN32
#include <WinSock2.h>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include "zrpc/base/log.h"
#include "zrpc/base/timer.h"
#include "zrpc/net/socket.h"

namespace zrpc {
class Channel;

class EventLoop;

class Select {
 public:
  typedef std::vector<Channel *> ChannelList;
  typedef std::unordered_map<SocketHandle, Channel *> ChannelMap;
  typedef std::vector<struct pollfd> EventList;

  Select(EventLoop *loop);

  ~Select();

  void EpollWait(ChannelList *active_channels, int32_t ms_time = 100);
  bool UpdateChannel(Channel *channel);
  bool RemoveChannel(Channel *channel);
  bool HasChannel(Channel *channel);

 private:
  void FillActiveChannels(int32_t num_events,
                          ChannelList *active_channels) const;

  ChannelMap channels_;
  EventList events_;
  EventLoop *loop_;

  fd_set rfds_;
  fd_set wfds_;
  fd_set efds_;
};

}  // namespace zrpc
#endif
