#include "zrpc/rpc/channel.h"

#include <algorithm>
#include <chrono>
#include <exception>

#include "zrpc/base/logger.h"
#include "zrpc/base/timer.h"
#include "zrpc/net/event_loop.h"

namespace zrpc {
namespace rpc {
namespace {

double NowSeconds() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

std::string HandlerKey(const std::string& service, const std::string& method) {
  return service + "." + method;
}

void InvokeHandler(const RpcHandler& handler, Context* ctx,
                   const std::string& request, std::string* response) {
  try {
    handler(ctx, request, response);
  } catch (const std::exception& ex) {
    ctx->SetFailed(ex.what(), ErrorCode::kInvalidResponse);
  } catch (...) {
    ctx->SetFailed("rpc handler threw an unknown exception",
                   ErrorCode::kInvalidResponse);
  }
}

void InvokeDoneSafely(const DoneCallback& done, Context* ctx,
                      const std::string& response) {
  if (!done) {
    return;
  }
  try {
    done(ctx, response);
  } catch (const std::exception& ex) {
    LOG_WARN << "rpc completion callback threw: " << ex.what();
  } catch (...) {
    LOG_WARN << "rpc completion callback threw an unknown exception";
  }
}

}  // namespace

Channel::Channel()
    : codec_(std::bind(&Channel::OnRpcMessage, this, std::placeholders::_1,
                       std::placeholders::_2)) {}

Channel::Channel(const std::shared_ptr<TcpConnection>& conn)
    : codec_(std::bind(&Channel::OnRpcMessage, this, std::placeholders::_1,
                       std::placeholders::_2)),
      conn_(conn) {}

Channel::~Channel() {
  closing_.store(true, std::memory_order_release);
  handlers_.store(nullptr, std::memory_order_release);
  FailAllOutstanding("rpc channel destroyed", ErrorCode::kTransport);
}

void Channel::PrepareShutdown() {
  closing_.store(true, std::memory_order_release);
  handlers_.store(nullptr, std::memory_order_release);
  FailAllOutstanding("rpc channel shutting down", ErrorCode::kTransport);
  std::shared_ptr<TcpConnection> conn = ConnectionSnapshot();
  if (conn && conn->GetLoop()) {
    std::weak_ptr<Channel> weak = weak_from_this();
    conn->GetLoop()->RunInLoop([weak, conn]() {
      if (auto self = weak.lock()) {
        if (conn->Connected()) {
          conn->Shutdown();
        }
        self->disconnected_.store(true, std::memory_order_release);
        {
          std::lock_guard<std::mutex> lk(self->mutex_);
          self->conn_.reset();
        }
      }
    });
  } else {
    OnDisconnect();
  }
}

void Channel::SetConnection(const std::shared_ptr<TcpConnection>& conn) {
  std::lock_guard<std::mutex> lk(mutex_);
  conn_ = conn;
  disconnected_.store(false, std::memory_order_release);
}

void Channel::OnDisconnect() {
  disconnected_.store(true, std::memory_order_release);
  FailAllOutstanding("connection closed", ErrorCode::kTransport);
  {
    std::lock_guard<std::mutex> lk(mutex_);
    conn_.reset();
  }
}

bool Channel::Connected() const {
  if (disconnected_.load(std::memory_order_acquire) ||
      closing_.load(std::memory_order_acquire)) {
    return false;
  }
  std::lock_guard<std::mutex> lk(mutex_);
  return conn_ && conn_->Connected();
}

std::shared_ptr<TcpConnection> Channel::ConnectionSnapshot() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return conn_;
}

std::shared_ptr<RpcMetrics> Channel::MetricsSnapshot() const {
  return std::atomic_load_explicit(&metrics_, std::memory_order_acquire);
}

void Channel::SetContextFailed(Context* ctx, const std::string& reason,
                               ErrorCode code) {
  if (ctx != nullptr) {
    ctx->SetFailed(reason, code);
  }
}

bool Channel::RemoveOutstanding(int64_t id, OutstandingCall* out) {
  std::unique_lock<std::mutex> lk(mutex_);
  auto it = outstandings_.find(id);
  if (it == outstandings_.end()) {
    return false;
  }
  if (out != nullptr) {
    *out = it->second;
  }
  outstandings_.erase(it);
  return true;
}

void Channel::FailOutstanding(int64_t id, const std::string& reason,
                            ErrorCode code) {
  OutstandingCall out;
  if (!RemoveOutstanding(id, &out)) {
    return;
  }

  if (out.timeout_timer) {
    std::shared_ptr<TcpConnection> conn = ConnectionSnapshot();
    if (conn) {
      conn->GetLoop()->CancelAfter(out.timeout_timer);
    }
  }

  SetContextFailed(out.ctx, reason, code);
  if (auto metrics = MetricsSnapshot()) {
    metrics->RecordFailure(code == ErrorCode::kTimeout);
  }
  InvokeDoneSafely(out.done, out.ctx, {});
  if (out.completed) {
    out.completed->store(true, std::memory_order_release);
  }
  if (out.sync) {
    sync_cv_.notify_all();
  }
}

void Channel::FailAllOutstanding(const std::string& reason, ErrorCode code) {
  std::map<int64_t, OutstandingCall> pending;
  {
    std::unique_lock<std::mutex> lk(mutex_);
    pending.swap(outstandings_);
  }

  for (auto& item : pending) {
    OutstandingCall& out = item.second;
    if (out.timeout_timer) {
      std::shared_ptr<TcpConnection> conn = ConnectionSnapshot();
      if (conn) {
        conn->GetLoop()->CancelAfter(out.timeout_timer);
      }
    }
    SetContextFailed(out.ctx, reason, code);
    if (auto metrics = MetricsSnapshot()) {
      metrics->RecordFailure(code == ErrorCode::kTimeout);
    }
    InvokeDoneSafely(out.done, out.ctx, {});
    if (out.completed) {
      out.completed->store(true, std::memory_order_release);
    }
  }
  sync_cv_.notify_all();
}

void Channel::ScheduleTimeout(int64_t id, double timeout_sec) {
  std::shared_ptr<TcpConnection> conn = ConnectionSnapshot();
  if (timeout_sec <= 0.0 || !conn) {
    return;
  }

  EventLoop* loop = conn->GetLoop();
  std::weak_ptr<Channel> weak = shared_from_this();
  std::shared_ptr<Timer> timer = loop->RunAfter(
      timeout_sec, false, [weak, id]() {
        if (auto self = weak.lock()) {
          self->OnCallTimeout(id);
        }
      });

  std::unique_lock<std::mutex> lk(mutex_);
  auto it = outstandings_.find(id);
  if (it != outstandings_.end()) {
    it->second.timeout_timer = std::move(timer);
    return;
  }
  lk.unlock();
  loop->CancelAfter(timer);
}

void Channel::OnCallTimeout(int64_t id) {
  FailOutstanding(id, "rpc call timeout", ErrorCode::kTimeout);
}

void Channel::Call(Context* ctx, const std::string& service,
                   const std::string& method, const std::string& request,
                   DoneCallback done) {
  if (closing_.load(std::memory_order_acquire)) {
    SetContextFailed(ctx, "channel closing", ErrorCode::kTransport);
    InvokeDoneSafely(done, ctx, {});
    return;
  }

  std::shared_ptr<TcpConnection> conn = ConnectionSnapshot();
  if (!conn || !conn->Connected()) {
    SetContextFailed(ctx, "not connected", ErrorCode::kTransport);
    InvokeDoneSafely(done, ctx, {});
    return;
  }

  const int64_t call_id = id_.fetch_add(1, std::memory_order_relaxed);
  OutstandingCall out;
  out.ctx = ctx;
  out.done = std::move(done);
  out.start_us = NowSeconds() * 1e6;

  {
    std::unique_lock<std::mutex> lk(mutex_);
    outstandings_.emplace(call_id, out);
  }

  if (auto metrics = MetricsSnapshot()) {
    metrics->RecordRequest();
  }

  Message message;
  message.type = MessageType::kRequest;
  message.id = call_id;
  message.service = service;
  message.method = method;
  message.body = request;
  if (!codec_.Send(conn, message)) {
    FailOutstanding(call_id, "failed to send rpc request", ErrorCode::kTransport);
    return;
  }

  const double timeout = ctx != nullptr ? ctx->TimeoutSeconds() : 0.0;
  ScheduleTimeout(call_id, timeout);
}

bool Channel::CallSync(Context* ctx, const std::string& service,
                       const std::string& method, const std::string& request,
                       std::string* response) {
  if (response == nullptr) {
    SetContextFailed(ctx, "null response", ErrorCode::kInvalidRequest);
    return false;
  }
  if (closing_.load(std::memory_order_acquire)) {
    SetContextFailed(ctx, "channel closing", ErrorCode::kTransport);
    return false;
  }

  std::shared_ptr<TcpConnection> conn = ConnectionSnapshot();
  if (!conn || !conn->Connected()) {
    SetContextFailed(ctx, "not connected", ErrorCode::kTransport);
    return false;
  }

  const int64_t call_id = id_.fetch_add(1, std::memory_order_relaxed);
  OutstandingCall out;
  out.ctx = ctx;
  out.sync = true;
  out.start_us = NowSeconds() * 1e6;
  out.completed = std::make_shared<std::atomic<bool>>(false);
  out.done = [response](Context* call_ctx, const std::string& body) {
    if (call_ctx != nullptr && !call_ctx->Failed()) {
      *response = body;
    }
  };

  {
    std::unique_lock<std::mutex> lk(mutex_);
    outstandings_.emplace(call_id, out);
  }

  if (auto metrics = MetricsSnapshot()) {
    metrics->RecordRequest();
  }

  Message message;
  message.type = MessageType::kRequest;
  message.id = call_id;
  message.service = service;
  message.method = method;
  message.body = request;
  if (!codec_.Send(conn, message)) {
    FailOutstanding(call_id, "failed to send rpc request", ErrorCode::kTransport);
    return false;
  }

  const double timeout =
      ctx != nullptr && ctx->TimeoutSeconds() > 0.0 ? ctx->TimeoutSeconds()
                                                    : 5.0;
  ScheduleTimeout(call_id, timeout);
  WaitSyncResponse(call_id, timeout, out.completed);

  return ctx == nullptr || !ctx->Failed();
}

void Channel::WaitSyncResponse(
    int64_t id, double timeout_sec,
    const std::shared_ptr<std::atomic<bool>>& completed) {
  if (!completed) {
    return;
  }
  std::shared_ptr<TcpConnection> conn = ConnectionSnapshot();
  EventLoop* loop = conn ? conn->GetLoop() : nullptr;
  const double deadline =
      timeout_sec > 0.0 ? NowSeconds() + timeout_sec : 0.0;

  if (loop != nullptr && loop->IsInLoopThread()) {
    bool timeout_requested = false;
    while (!completed->load(std::memory_order_acquire)) {
      int wait_ms = 100;
      if (deadline > 0.0 && !timeout_requested) {
        const double remain = deadline - NowSeconds();
        if (remain <= 0.0) {
          FailOutstanding(id, "rpc call timeout", ErrorCode::kTimeout);
          timeout_requested = true;
          continue;
        }
        wait_ms = static_cast<int>(remain * 1000.0);
        if (wait_ms < 1) {
          wait_ms = 1;
        }
      }
      loop->PollOnce(wait_ms);
    }
    return;
  }

  std::unique_lock<std::mutex> lk(mutex_);
  const auto wait_deadline = std::chrono::steady_clock::now() +
                             std::chrono::duration<double>(timeout_sec);
  if (!sync_cv_.wait_until(
          lk, wait_deadline,
          [&completed]() {
            return completed->load(std::memory_order_acquire);
          })) {
    lk.unlock();
    FailOutstanding(id, "rpc call timeout", ErrorCode::kTimeout);
    lk.lock();
    sync_cv_.wait(lk, [&completed]() {
      return completed->load(std::memory_order_acquire);
    });
  }
}

void Channel::OnMessage(const std::shared_ptr<TcpConnection>& conn,
                        Buffer* buf) {
  codec_.OnMessage(conn, buf);
}

void Channel::OnRpcMessage(const std::shared_ptr<TcpConnection>& conn,
                           const Message& message) {
  if (message.type == MessageType::kResponse) {
    OutstandingCall out;
    if (!RemoveOutstanding(message.id, &out)) {
      return;
    }

    if (out.timeout_timer) {
      std::shared_ptr<TcpConnection> active_conn = ConnectionSnapshot();
      if (active_conn) {
        active_conn->GetLoop()->CancelAfter(out.timeout_timer);
      }
    }

    CompleteResponse(out, message);
    return;
  }

  if (message.type != MessageType::kRequest) {
    return;
  }

  if (closing_.load(std::memory_order_acquire)) {
    Message response;
    response.type = MessageType::kResponse;
    response.id = message.id;
    response.error = ErrorCode::kTransport;
    codec_.Send(conn, response);
    return;
  }

  const std::map<std::string, RpcHandler>* handlers =
      handlers_.load(std::memory_order_acquire);
  if (handlers == nullptr) {
    Message response;
    response.type = MessageType::kResponse;
    response.id = message.id;
    response.error = ErrorCode::kNoService;
    codec_.Send(conn, response);
    if (auto metrics = MetricsSnapshot()) {
      metrics->RecordServerError();
    }
    return;
  }

  const auto it = handlers->find(HandlerKey(message.service, message.method));
  if (it == handlers->end()) {
    const std::string service_prefix = message.service + ".";
    const bool service_exists =
        !message.service.empty() &&
        std::any_of(handlers->begin(), handlers->end(),
                    [&service_prefix](const auto& item) {
                      return item.first.compare(0, service_prefix.size(),
                                                service_prefix) == 0;
                    });
    Message response;
    response.type = MessageType::kResponse;
    response.id = message.id;
    response.error =
        service_exists ? ErrorCode::kNoMethod : ErrorCode::kNoService;
    codec_.Send(conn, response);
    if (auto metrics = MetricsSnapshot()) {
      metrics->RecordServerError();
    }
    return;
  }

  if (auto metrics = MetricsSnapshot()) {
    metrics->RecordServerRequest();
  }

  server_inflight_.fetch_add(1, std::memory_order_relaxed);
  DispatchRequest(conn, message, it->second);
}

void Channel::DispatchRequest(const std::shared_ptr<TcpConnection>& conn,
                              const Message& message,
                              const RpcHandler& handler) {
  const double start = NowSeconds() * 1e6;

  if (worker_pool_ != nullptr && worker_pool_->Active()) {
    std::weak_ptr<Channel> weak = weak_from_this();
    auto worker_pool = worker_pool_;
    const bool posted =
        worker_pool->Post([weak, conn, message, handler, start]() {
          auto ctx = std::make_unique<Context>();
          std::string response_body;
          InvokeHandler(handler, ctx.get(), message.body, &response_body);

          Message response;
          response.type = MessageType::kResponse;
          response.id = message.id;
          if (ctx->Failed()) {
            response.error = ctx->GetErrorCode();
            response.body = ctx->ErrorText();
          } else {
            response.body = std::move(response_body);
          }

          EventLoop* loop = conn ? conn->GetLoop() : nullptr;
          if (loop == nullptr) {
            if (auto self = weak.lock()) {
              self->server_inflight_.fetch_sub(1,
                                               std::memory_order_relaxed);
            }
            return;
          }

          const bool failed = ctx->Failed();
          loop->RunInLoop(
              [weak, conn, response = std::move(response), start,
               failed]() mutable {
                if (auto self = weak.lock()) {
                  self->SendResponse(conn, std::move(response), start, failed);
                }
              });
        });
    if (posted) {
      return;
    }

    Message response;
    response.type = MessageType::kResponse;
    response.id = message.id;
    response.error = ErrorCode::kTransport;
    response.body = "rpc server overloaded or shutting down";
    SendResponse(conn, std::move(response), start, true);
    return;
  }

  auto ctx = std::make_unique<Context>();
  std::string response_body;
  InvokeHandler(handler, ctx.get(), message.body, &response_body);

  Message response;
  response.type = MessageType::kResponse;
  response.id = message.id;
  if (ctx->Failed()) {
    response.error = ctx->GetErrorCode();
    response.body = ctx->ErrorText();
  } else {
    response.body = std::move(response_body);
  }
  SendResponse(conn, std::move(response), start, ctx->Failed());
}

void Channel::SendResponse(const std::shared_ptr<TcpConnection>& conn,
                           Message response, double start_us, bool failed) {
  if (failed) {
    if (auto metrics = MetricsSnapshot()) {
      metrics->RecordServerError();
    }
  } else if (auto metrics = MetricsSnapshot()) {
    metrics->RecordSuccess(NowSeconds() * 1e6 - start_us);
  }
  codec_.Send(conn, response);
  server_inflight_.fetch_sub(1, std::memory_order_relaxed);
}

void Channel::CompleteResponse(const OutstandingCall& out,
                               const Message& message) {
  const double latency_us =
      out.start_us > 0.0 ? NowSeconds() * 1e6 - out.start_us : 0.0;

  if (message.error != ErrorCode::kOk) {
    const std::string reason =
        message.body.empty() ? ErrorCodeName(message.error) : message.body;
    SetContextFailed(out.ctx, reason, message.error);
    if (auto metrics = MetricsSnapshot()) {
      metrics->RecordFailure(message.error == ErrorCode::kTimeout);
    }
    InvokeDoneSafely(out.done, out.ctx, {});
    if (out.completed) {
      out.completed->store(true, std::memory_order_release);
    }
    if (out.sync) {
      sync_cv_.notify_all();
    }
    return;
  }

  if (auto metrics = MetricsSnapshot()) {
    metrics->RecordSuccess(latency_us);
  }
  InvokeDoneSafely(out.done, out.ctx, message.body);
  if (out.completed) {
    out.completed->store(true, std::memory_order_release);
  }
  if (out.sync) {
    sync_cv_.notify_all();
  }
}

}  // namespace rpc
}  // namespace zrpc
