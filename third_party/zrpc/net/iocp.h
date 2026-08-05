#pragma once

#ifdef _WIN32
#include <WinSock2.h>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <memory>
#include <unordered_map>
#include <vector>

#include "zrpc/base/log.h"
#include "zrpc/net/socket.h"

namespace zrpc {
class Channel;

class EventLoop;

class Iocp {
 public:
  typedef std::vector<Channel *> ChannelList;
  typedef std::unordered_map<SocketHandle, Channel *> ChannelMap;

  Iocp(EventLoop *loop);

  ~Iocp();

  void EpollWait(ChannelList *active_channels, int32_t ms_time = 100);
  bool HasChannel(Channel *channel);
  bool UpdateChannel(Channel *channel);
  bool RemoveChannel(Channel *channel);

 private:
  Iocp(const Iocp &);

  void operator=(const Iocp &);

  bool Update(Channel *channel);
  void FillActiveChannels(ChannelList *active_channels) const;
  void ReleaseSocketContext(SocketHandle fd);

  friend class EventLoop;

  struct ContextHolder;
  std::unique_ptr<ContextHolder> holder_;
  EventLoop *loop_;
  HANDLE iocpfd_;
  ChannelMap channels;
};

}  // namespace zrpc
#endif
