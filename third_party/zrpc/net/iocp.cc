#ifdef _WIN32

#include "zrpc/net/iocp.h"

#include <algorithm>
#include <cstring>

#include "zrpc/net/channel.h"
#include "zrpc/net/event_loop.h"
#include "zrpc/base/logger.h"

namespace zrpc {
struct ChannelContext {
  Channel* channel{nullptr};
  SocketHandle fd{kInvalidSocket};
  OVERLAPPED read_overlapped{};
  OVERLAPPED write_overlapped{};
  WSABUF read_buf{};
  WSABUF write_buf{};
  char read_dummy{};
  char write_dummy{};
  bool read_pending{false};
  bool write_pending{false};
  bool iocp_associated{false};
  bool is_listen_socket{false};
  bool is_connecting{false};
};

struct Iocp::ContextHolder {
  std::unordered_map<SocketHandle, std::unique_ptr<ChannelContext>> contexts;
  std::vector<std::unique_ptr<ChannelContext>> retired_contexts;
  std::unordered_map<Channel*, int32_t> active_revents;
  int32_t listen_socket_count{0};
  int32_t connecting_socket_count{0};
};

namespace {
const int32_t kNew = -1;
const int32_t kAdded = 1;
const int32_t kDeleted = 2;

bool IsListeningSocket(SOCKET fd) {
  int acceptconn = 0;
  int len = sizeof(acceptconn);
  if (::getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN,
                   reinterpret_cast<char*>(&acceptconn), &len) != 0) {
    return false;
  }
  return acceptconn != 0;
}

using ContextMap =
    std::unordered_map<SocketHandle, std::unique_ptr<ChannelContext>>;
using RetiredContexts = std::vector<std::unique_ptr<ChannelContext>>;
using ActiveRevents = std::unordered_map<Channel*, int32_t>;

void AddActiveEvent(ActiveRevents* active_revents, Channel* channel, int32_t revents) {
  (*active_revents)[channel] |= revents;
}

void UpdateContextFlags(ChannelContext* ctx, Channel* channel, int32_t* listen_socket_count,
                        int32_t* connecting_socket_count) {
  const bool was_listen = ctx->is_listen_socket;
  const bool was_connecting = ctx->is_connecting;

  SOCKET fd = static_cast<SOCKET>(ctx->fd);
  const bool listen = IsListeningSocket(fd);
  const bool connecting = channel != nullptr && channel->IsWriting() &&
                          !channel->IsReading() && !listen;

  ctx->is_listen_socket = listen;
  ctx->is_connecting = connecting;

  if (listen != was_listen) {
    *listen_socket_count += listen ? 1 : -1;
  }
  if (connecting != was_connecting) {
    *connecting_socket_count += connecting ? 1 : -1;
  }
}

bool IsActiveContext(const ContextMap& contexts, const ChannelContext* ctx) {
  for (const auto& item : contexts) {
    if (item.second.get() == ctx) {
      return true;
    }
  }
  return false;
}

bool HandleRetiredCompletion(RetiredContexts* retired_contexts,
                             ChannelContext* ctx, OVERLAPPED* overlapped) {
  for (auto& retired : *retired_contexts) {
    if (retired.get() != ctx) {
      continue;
    }
    if (overlapped == &ctx->read_overlapped) {
      ctx->read_pending = false;
    } else if (overlapped == &ctx->write_overlapped) {
      ctx->write_pending = false;
    }
    return true;
  }
  return false;
}

void ReclaimRetiredContexts(RetiredContexts* retired_contexts) {
  retired_contexts->erase(
      std::remove_if(retired_contexts->begin(), retired_contexts->end(),
                     [](const std::unique_ptr<ChannelContext>& ctx) {
                       return !ctx->read_pending && !ctx->write_pending;
                     }),
      retired_contexts->end());
}

void HandleCompletion(ActiveRevents* active_revents, const ContextMap& contexts,
                      ChannelContext* ctx, OVERLAPPED* overlapped, DWORD bytes, BOOL ok) {
  if (!IsActiveContext(contexts, ctx)) {
    return;
  }
  Channel* channel = ctx->channel;
  if (channel == nullptr) {
    return;
  }
  if (overlapped == &ctx->read_overlapped) {
    ctx->read_pending = false;
    if (!ok) {
      int err = static_cast<int>(::GetLastError());
      if (err == WSAECONNRESET || err == WSAESHUTDOWN || err == WSAECONNABORTED) {
        AddActiveEvent(active_revents, channel, POLLERR | POLLHUP);
      } else if (err != ERROR_OPERATION_ABORTED) {
        AddActiveEvent(active_revents, channel, POLLERR);
      }
      return;
    }
    AddActiveEvent(active_revents, channel, POLLIN);
    (void)bytes;
  } else if (overlapped == &ctx->write_overlapped) {
    ctx->write_pending = false;
    if (!ok) {
      int err = static_cast<int>(::GetLastError());
      if (err == WSAECONNRESET || err == WSAESHUTDOWN || err == WSAECONNABORTED) {
        AddActiveEvent(active_revents, channel, POLLERR | POLLHUP);
      } else if (err != ERROR_OPERATION_ABORTED) {
        AddActiveEvent(active_revents, channel, POLLERR);
      }
      return;
    }
    AddActiveEvent(active_revents, channel, POLLOUT);
    (void)bytes;
  }
}

void DrainCompletions(HANDLE iocpfd, ActiveRevents* active_revents,
                      const ContextMap& contexts,
                      RetiredContexts* retired_contexts) {
  while (true) {
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    LPOVERLAPPED overlapped = nullptr;
    BOOL ok = ::GetQueuedCompletionStatus(iocpfd, &bytes, &key, &overlapped, 0);
    if (overlapped == nullptr) {
      break;
    }
    auto* ctx = reinterpret_cast<ChannelContext*>(key);
    if (IsActiveContext(contexts, ctx)) {
      HandleCompletion(active_revents, contexts, ctx, overlapped, bytes, ok);
    } else {
      HandleRetiredCompletion(retired_contexts, ctx, overlapped);
    }
  }
  ReclaimRetiredContexts(retired_contexts);
}

ChannelContext* GetOrCreateContext(ContextMap* contexts, Channel* channel,
                                  int32_t* listen_socket_count,
                                  int32_t* connecting_socket_count) {
  SocketHandle fd = channel->Getfd();
  auto it = contexts->find(fd);
  if (it != contexts->end()) {
    ChannelContext* ctx = it->second.get();
    ctx->channel = channel;
    ctx->fd = fd;
    return ctx;
  }

  auto ctx = std::make_unique<ChannelContext>();
  ctx->channel = channel;
  ctx->fd = fd;
  ChannelContext* raw = ctx.get();
  contexts->emplace(fd, std::move(ctx));
  UpdateContextFlags(raw, channel, listen_socket_count, connecting_socket_count);
  return raw;
}

bool AssociateChannel(HANDLE iocpfd, Channel* channel, ChannelContext* ctx) {
  ctx->channel = channel;
  if (ctx->iocp_associated) {
    return true;
  }

  HANDLE port = ::CreateIoCompletionPort(
      reinterpret_cast<HANDLE>(static_cast<SOCKET>(ctx->fd)), iocpfd,
      reinterpret_cast<ULONG_PTR>(ctx), 0);
  if (port != NULL) {
    ctx->iocp_associated = true;
    return true;
  }

  DWORD err = ::GetLastError();
  LOG_WARN << "Iocp::Update failed, fd=" << ctx->fd << " err=" << err;
  return false;
}

void DisarmRead(ChannelContext* ctx) {
  if (!ctx->read_pending) {
    return;
  }
  SOCKET fd = static_cast<SOCKET>(ctx->fd);
  ::CancelIoEx(reinterpret_cast<HANDLE>(fd), &ctx->read_overlapped);
}

void DisarmWrite(ChannelContext* ctx) {
  if (!ctx->write_pending) {
    return;
  }
  SOCKET fd = static_cast<SOCKET>(ctx->fd);
  ::CancelIoEx(reinterpret_cast<HANDLE>(fd), &ctx->write_overlapped);
}

void ArmRead(ActiveRevents* active_revents, ChannelContext* ctx) {
  if (ctx->read_pending || ctx->channel == nullptr || !ctx->channel->IsReading()) {
    return;
  }

  SOCKET fd = static_cast<SOCKET>(ctx->fd);
  if (IsListeningSocket(fd)) {
    return;
  }

  memset(&ctx->read_overlapped, 0, sizeof(ctx->read_overlapped));
  ctx->read_buf.buf = &ctx->read_dummy;
  ctx->read_buf.len = 0;

  DWORD flags = 0;
  DWORD bytes = 0;
  int ret = ::WSARecv(fd, &ctx->read_buf, 1, &bytes, &flags, &ctx->read_overlapped, NULL);
  if (ret == 0) {
    ctx->read_pending = true;
    return;
  }

  int err = ::WSAGetLastError();
  if (err == WSA_IO_PENDING) {
    ctx->read_pending = true;
  } else if (err == WSAECONNRESET || err == WSAESHUTDOWN || err == WSAECONNABORTED) {
    AddActiveEvent(active_revents, ctx->channel, POLLERR | POLLHUP);
  } else {
    AddActiveEvent(active_revents, ctx->channel, POLLERR);
  }
}

void ArmWrite(ActiveRevents* active_revents, ChannelContext* ctx) {
  if (ctx->channel == nullptr || !ctx->channel->IsWriting()) {
    return;
  }

  SOCKET fd = static_cast<SOCKET>(ctx->fd);
  if (IsListeningSocket(fd)) {
    return;
  }

  fd_set wfds;
  fd_set efds;
  FD_ZERO(&wfds);
  FD_ZERO(&efds);
  FD_SET(fd, &wfds);
  FD_SET(fd, &efds);
  timeval tv = {0, 0};
  const int n = ::select(0, nullptr, &wfds, &efds, &tv);
  if (n == SOCKET_ERROR) {
    AddActiveEvent(active_revents, ctx->channel, POLLERR);
  } else if (n > 0) {
    if (FD_ISSET(fd, &efds)) {
      AddActiveEvent(active_revents, ctx->channel, POLLERR);
    }
    if (FD_ISSET(fd, &wfds)) {
      AddActiveEvent(active_revents, ctx->channel, POLLOUT);
    }
  }
}

void PollListenSockets(ActiveRevents* active_revents, const ContextMap& contexts,
                       int32_t listen_socket_count) {
  if (listen_socket_count <= 0) {
    return;
  }
  for (const auto& item : contexts) {
    ChannelContext* ctx = item.second.get();
    Channel* channel = ctx->channel;
    if (!ctx->is_listen_socket || channel == nullptr || !channel->IsReading()) {
      continue;
    }

    SOCKET fd = static_cast<SOCKET>(ctx->fd);
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    timeval tv = {0, 0};
    int n = ::select(0, &rfds, nullptr, nullptr, &tv);
    if (n > 0) {
      AddActiveEvent(active_revents, channel, POLLIN);
    }
  }
}

void ArmPendingOps(HANDLE iocpfd, ContextMap* contexts, ActiveRevents* active_revents,
                   int32_t* listen_socket_count, int32_t* connecting_socket_count) {
  (void)iocpfd;
  (void)connecting_socket_count;
  PollListenSockets(active_revents, *contexts, *listen_socket_count);
  // 可写事件使用 select 探测真实发送缓冲区状态，避免零字节 WSASend 忙循环。
  for (auto& item : *contexts) {
    ChannelContext* ctx = item.second.get();
    Channel* channel = ctx->channel;
    if (channel == nullptr) {
      continue;
    }
    if (!channel->IsReading()) {
      DisarmRead(ctx);
    }
    if (!channel->IsWriting()) {
      DisarmWrite(ctx);
    }
    ArmRead(active_revents, ctx);
    ArmWrite(active_revents, ctx);
  }
}

bool HasSelectPolledSockets(const ContextMap& contexts) {
  for (const auto& item : contexts) {
    const ChannelContext* ctx = item.second.get();
    if (ctx->channel != nullptr &&
        ((ctx->is_listen_socket && ctx->channel->IsReading()) ||
         ctx->channel->IsWriting())) {
      return true;
    }
  }
  return false;
}

void ClearContextCounts(ChannelContext* ctx, int32_t* listen_socket_count,
                        int32_t* connecting_socket_count) {
  if (ctx->is_listen_socket) {
    --(*listen_socket_count);
  }
  if (ctx->is_connecting) {
    --(*connecting_socket_count);
  }
  ctx->is_listen_socket = false;
  ctx->is_connecting = false;
}
}  // namespace

Iocp::Iocp(EventLoop* loop)
    : holder_(std::make_unique<ContextHolder>()),
      loop_(loop),
      iocpfd_(::CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0)) {
  if (iocpfd_ == NULL) {
    LOG_FATAL << "Iocp::Iocp CreateIoCompletionPort failed";
  }
}

Iocp::~Iocp() {
  for (auto& item : holder_->contexts) {
    ChannelContext* ctx = item.second.get();
    DisarmRead(ctx);
    DisarmWrite(ctx);
    ctx->channel = nullptr;
    holder_->retired_contexts.push_back(std::move(item.second));
  }
  holder_->contexts.clear();

  for (int attempt = 0;
       attempt < 1000 && !holder_->retired_contexts.empty(); ++attempt) {
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    LPOVERLAPPED overlapped = nullptr;
    const BOOL ok =
        ::GetQueuedCompletionStatus(iocpfd_, &bytes, &key, &overlapped, 1);
    if (overlapped != nullptr) {
      auto* ctx = reinterpret_cast<ChannelContext*>(key);
      HandleRetiredCompletion(&holder_->retired_contexts, ctx, overlapped);
      ReclaimRetiredContexts(&holder_->retired_contexts);
    } else if (!ok && ::GetLastError() != WAIT_TIMEOUT) {
      break;
    }
  }

  if (!holder_->retired_contexts.empty()) {
    // 极端取消失败时保留 OVERLAPPED 内存到进程结束，避免内核完成包写入悬空地址。
    auto* quarantine =
        new std::vector<std::unique_ptr<ChannelContext>>();
    quarantine->swap(holder_->retired_contexts);
  }
  if (iocpfd_ != NULL) {
    ::CloseHandle(iocpfd_);
    iocpfd_ = NULL;
  }
}

void Iocp::EpollWait(ChannelList* active_channels, int32_t ms_time) {
  auto timer_queue = loop_->GetTimerQueue();
  const int64_t timer_ms = timer_queue->GetTimeout();
  if (ms_time < 0 || timer_ms < ms_time) {
    ms_time = static_cast<int32_t>(timer_ms);
  }

  holder_->active_revents.clear();
  ArmPendingOps(iocpfd_, &holder_->contexts, &holder_->active_revents,
                &holder_->listen_socket_count, &holder_->connecting_socket_count);

  DWORD wait_ms = holder_->active_revents.empty()
                      ? (ms_time < 0 ? INFINITE
                                     : static_cast<DWORD>(ms_time))
                      : 0;
  if ((wait_ms == INFINITE || wait_ms > 10) &&
      HasSelectPolledSockets(holder_->contexts)) {
    wait_ms = 10;
  }
  bool first_wait = true;

  while (true) {
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    LPOVERLAPPED overlapped = nullptr;

    BOOL ok = ::GetQueuedCompletionStatus(iocpfd_, &bytes, &key, &overlapped,
                                          first_wait ? wait_ms : 0);
    first_wait = false;

    if (overlapped != nullptr) {
      auto* ctx = reinterpret_cast<ChannelContext*>(key);
      if (IsActiveContext(holder_->contexts, ctx)) {
        HandleCompletion(&holder_->active_revents, holder_->contexts, ctx,
                         overlapped, bytes, ok);
      } else {
        HandleRetiredCompletion(&holder_->retired_contexts, ctx, overlapped);
      }
      continue;
    }

    DWORD err = ::GetLastError();
    if (!ok) {
      if (err == WAIT_TIMEOUT) {
        break;
      }
      if (err == ERROR_OPERATION_ABORTED) {
        continue;
      }
      break;
    }
    break;
  }

  DrainCompletions(iocpfd_, &holder_->active_revents, holder_->contexts,
                   &holder_->retired_contexts);
  FillActiveChannels(active_channels);
  loop_->HandlerTimerQueue();
}

void Iocp::FillActiveChannels(ChannelList* active_channels) const {
  for (const auto& item : holder_->active_revents) {
    item.first->SetRevents(item.second);
    active_channels->push_back(item.first);
  }
}

bool Iocp::HasChannel(Channel* channel) {
  loop_->AssertInLoopThread();
  auto it = channels.find(channel->Getfd());
  return it != channels.end() && it->second == channel;
}

bool Iocp::Update(Channel* channel) {
  ChannelContext* ctx =
      GetOrCreateContext(&holder_->contexts, channel, &holder_->listen_socket_count,
                         &holder_->connecting_socket_count);
  if (!AssociateChannel(iocpfd_, channel, ctx)) {
    ClearContextCounts(ctx, &holder_->listen_socket_count,
                       &holder_->connecting_socket_count);
    holder_->contexts.erase(channel->Getfd());
    return false;
  }

  if (channel->IsNoneEvent()) {
    DisarmRead(ctx);
    DisarmWrite(ctx);
    return true;
  }

  if (!channel->IsReading()) {
    DisarmRead(ctx);
  }
  if (!channel->IsWriting()) {
    DisarmWrite(ctx);
  }
  UpdateContextFlags(ctx, channel, &holder_->listen_socket_count,
                     &holder_->connecting_socket_count);
  return true;
}

bool Iocp::UpdateChannel(Channel* channel) {
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
    if (!Update(channel)) {
      return false;
    }
    if (index == kNew) {
      channels[fd] = channel;
    }
    channel->SetIndex(kAdded);
  } else {
    SocketHandle fd = channel->Getfd();
    (void)fd;
    assert(channels.find(fd) != channels.end());
    assert(channels[fd] == channel);
    assert(index == kAdded);
    if (channel->IsNoneEvent()) {
      if (!Update(channel)) {
        return false;
      }
      channel->SetIndex(kDeleted);
    } else {
      if (!Update(channel)) {
        return false;
      }
    }
  }
  return true;
}

bool Iocp::RemoveChannel(Channel* channel) {
  loop_->AssertInLoopThread();
  SocketHandle fd = channel->Getfd();
  int32_t index = channel->GetIndex();
  assert(channels.find(fd) != channels.end());
  assert(channels[fd] == channel);
  assert(channel->IsNoneEvent());
  assert(index == kAdded || index == kDeleted);

  size_t n = channels.erase(fd);
  assert(n == 1);
  (void)n;

  auto ctx_it = holder_->contexts.find(fd);
  if (ctx_it != holder_->contexts.end()) {
    DisarmRead(ctx_it->second.get());
    DisarmWrite(ctx_it->second.get());
    ChannelContext* ctx = ctx_it->second.get();
    ClearContextCounts(ctx, &holder_->listen_socket_count,
                       &holder_->connecting_socket_count);
    ctx->channel = nullptr;
  }

  channel->SetIndex(kNew);
  return true;
}

void Iocp::ReleaseSocketContext(SocketHandle fd) {
  loop_->AssertInLoopThread();
  auto ctx_it = holder_->contexts.find(fd);
  if (ctx_it == holder_->contexts.end()) {
    return;
  }

  ChannelContext* ctx = ctx_it->second.get();
  DisarmRead(ctx);
  DisarmWrite(ctx);
  ClearContextCounts(ctx, &holder_->listen_socket_count,
                      &holder_->connecting_socket_count);
  ctx->channel = nullptr;
  holder_->retired_contexts.push_back(std::move(ctx_it->second));
  holder_->contexts.erase(ctx_it);
  DrainCompletions(iocpfd_, &holder_->active_revents, holder_->contexts,
                   &holder_->retired_contexts);
}

}  // namespace zrpc
#endif
