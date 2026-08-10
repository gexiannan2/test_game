#include "zrpc/rpc/client.h"

#include <cmath>
#include <exception>
#include <stdexcept>

#include "zrpc/base/logger.h"
#include "zrpc/base/timer.h"

namespace zrpc {
namespace rpc {
namespace {

void InvokeAsyncCallbackSafely(const AsyncCallback& callback,
                               const Reply& reply) {
  if (!callback) {
    return;
  }
  try {
    callback(reply);
  } catch (const std::exception& ex) {
    LOG_WARN << "rpc async callback threw: " << ex.what();
  } catch (...) {
    LOG_WARN << "rpc async callback threw an unknown exception";
  }
}

}  // namespace

Client::Client(EventLoop* loop, const std::string& ip, uint16_t port,
               const ClientOptions& options)
    : loop_(loop),
      options_(options),
      breaker_(std::make_shared<RpcCircuitBreaker>(options.breaker)) {
  discovery_ = std::make_shared<StaticServiceDiscovery>(
      std::vector<RpcEndpoint>{RpcEndpoint(ip, port)});
  Init(options);
}

Client::Client(EventLoop* loop, std::shared_ptr<ServiceDiscovery> discovery,
               const ClientOptions& options)
    : loop_(loop),
      options_(options),
      discovery_(std::move(discovery)),
      breaker_(std::make_shared<RpcCircuitBreaker>(options.breaker)) {
  Init(options);
}

void Client::Init(const ClientOptions& options) {
  if (loop_ == nullptr) {
    throw std::invalid_argument("rpc client requires a valid event loop");
  }
  if (!discovery_) {
    throw std::invalid_argument("rpc client requires service discovery");
  }
  options_ = options;
  if (!std::isfinite(options_.defaults.timeout_seconds) ||
      options_.defaults.timeout_seconds <= 0.0) {
    options_.defaults.timeout_seconds = 5.0;
  }
  if (!std::isfinite(options_.discovery_refresh_seconds) ||
      options_.discovery_refresh_seconds < 0.0) {
    options_.discovery_refresh_seconds = 0.0;
  }
  metrics_ = std::make_shared<RpcMetrics>();

  const RpcEndpoint preferred =
      discovery_ ? discovery_->Preferred() : RpcEndpoint{};
  pool_ = std::make_shared<ConnectionPool>(loop_, preferred, options_.pool_size);

  if (options_.discovery_refresh_seconds > 0.0 && discovery_ && loop_) {
    auto discovery = discovery_;
    auto pool = pool_;
    discovery_timer_ = loop_->RunAfter(
        options_.discovery_refresh_seconds, true,
        [discovery, pool]() {
          if (!discovery || !pool) {
            return;
          }
          const RpcEndpoint endpoint = discovery->Preferred();
          if (endpoint.ip.empty() || endpoint.port == 0) {
            return;
          }
          if (pool->endpoint() == endpoint) {
            return;
          }
          pool->UpdateEndpoint(endpoint);
          pool->Connect(false);
        });
  }
}

Client::~Client() { Shutdown(); }

Stub Client::StubFor(const std::string& service, const std::string& method) {
  return Stub(this, service, method);
}

Reply Client::Call(ProtocolId id, const std::string& body,
                   const CallOptions& options) {
  return InvokeSync(kProtocolService, ProtocolMethodName(id), body,
                    MergeOptions(options));
}

void Client::AsyncCall(ProtocolId id, const std::string& body,
                       AsyncCallback callback, const CallOptions& options) {
  InvokeAsync(kProtocolService, ProtocolMethodName(id), body, std::move(callback),
              MergeOptions(options));
}

void Client::SetDefaultCallOptions(CallOptions options) {
  if (!std::isfinite(options.timeout_seconds) ||
      options.timeout_seconds <= 0.0) {
    options.timeout_seconds = 5.0;
  }
  options_.defaults = std::move(options);
}

void Client::SetConnectionCallback(ConnectionCallback cb) {
  user_cb_ = std::move(cb);
  if (pool_) {
    pool_->SetConnectionCallback(user_cb_);
  }
}

void Client::EnableRetry() {
  if (pool_) {
    pool_->EnableRetry();
  }
}

void Client::Connect(bool wait) {
  if (!async_enabled_ ||
      !async_enabled_->load(std::memory_order_acquire)) {
    async_enabled_ = std::make_shared<std::atomic<bool>>(true);
  }
  if (pool_) {
    pool_->Connect(wait);
  }
}

void Client::Shutdown() {
  if (async_enabled_) {
    async_enabled_->store(false, std::memory_order_release);
  }
  if (discovery_timer_ && loop_) {
    auto timer = discovery_timer_;
    discovery_timer_.reset();
    if (loop_->IsInLoopThread()) {
      loop_->CancelAfter(timer);
    } else {
      loop_->RunInLoop([loop = loop_, timer]() { loop->CancelAfter(timer); });
    }
  }
  if (pool_) {
    pool_->Shutdown();
  }
}

RpcMetricsSnapshot Client::GetMetrics() const {
  return metrics_ ? metrics_->Snapshot() : RpcMetricsSnapshot{};
}

std::string Client::MetricsString() const {
  return metrics_ ? metrics_->ToString() : "rpc_metrics{}";
}

CallOptions Client::MergeOptions(const CallOptions& options) const {
  CallOptions merged = options_.defaults;
  if (std::isfinite(options.timeout_seconds) &&
      options.timeout_seconds > 0.0) {
    merged.timeout_seconds = options.timeout_seconds;
  }
  return merged;
}

Reply Client::MakeReply(Context* ctx, const std::string& body) {
  if (ctx == nullptr || !ctx->Failed()) {
    return Reply::Ok(body);
  }
  return Reply::Error(ctx->GetErrorCode(), ctx->ErrorText());
}

void Client::CallOnce(Context* ctx, const std::string& service,
                      const std::string& method, const std::string& request,
                      std::string* response) {
  ChannelPtr channel = pool_->Acquire();
  if (!channel || !channel->Connected()) {
    if (ctx != nullptr) {
      ctx->SetFailed("no available connection", ErrorCode::kTransport);
    }
    if (channel) {
      pool_->Release(channel);
    }
    return;
  }
  channel->SetMetrics(metrics_);
  if (response != nullptr) {
    channel->CallSync(ctx, service, method, request, response);
  } else {
    channel->Call(ctx, service, method, request, {});
  }
  pool_->Release(channel);
}

Reply Client::InvokeSync(const std::string& service, const std::string& method,
                         const std::string& request,
                         const CallOptions& options) {
  if (!breaker_->AllowRequest()) {
    metrics_->RecordRejected();
    return Reply::Error(ErrorCode::kRejected, "circuit breaker open");
  }

  Context ctx;
  ctx.SetTimeout(options.timeout_seconds);

  uint32_t attempt = 0;
  while (true) {
    ctx.Reset();

    std::string body;
    CallOnce(&ctx, service, method, request, &body);
    if (!ctx.Failed()) {
      breaker_->RecordSuccess();
      return Reply::Ok(std::move(body));
    }

    breaker_->RecordFailure();
    if (!options_.retry.ShouldRetry(&ctx, attempt)) {
      return MakeReply(&ctx, body);
    }
    options_.retry.SleepBeforeRetry(++attempt);
  }
}

void Client::StartAsyncAttempt(
    const std::shared_ptr<AsyncRetryState>& state) {
  if (!state || state->completed.load(std::memory_order_acquire)) {
    return;
  }
  if (!state->enabled ||
      !state->enabled->load(std::memory_order_acquire)) {
    CompleteAsync(
        state,
        Reply::Error(ErrorCode::kTransport, "rpc client shut down"));
    return;
  }
  if (!state->breaker->AllowRequest()) {
    state->metrics->RecordRejected();
    CompleteAsync(
        state,
        Reply::Error(ErrorCode::kRejected, "circuit breaker open"));
    return;
  }

  state->ctx = std::make_shared<Context>();
  state->ctx->SetTimeout(state->options.timeout_seconds);

  ChannelPtr channel = state->pool->Acquire();
  if (!channel || !channel->Connected()) {
    state->ctx->SetFailed("no available connection", ErrorCode::kTransport);
    if (channel) {
      state->pool->Release(channel);
    }
    FinishAsyncAttempt(state, state->ctx.get(), {});
    return;
  }

  channel->SetMetrics(state->metrics);
  channel->Call(
      state->ctx.get(), state->service, state->method, state->request,
      [state, channel](Context* call_ctx, const std::string& body) {
        state->pool->Release(channel);
        FinishAsyncAttempt(state, call_ctx, body);
      });
}

void Client::FinishAsyncAttempt(
    const std::shared_ptr<AsyncRetryState>& state, Context* ctx,
    const std::string& body) {
  const Reply reply = MakeReply(ctx, body);
  if (reply.ok()) {
    state->breaker->RecordSuccess();
    CompleteAsync(state, reply);
    return;
  }

  state->breaker->RecordFailure();
  if (!state->enabled ||
      !state->enabled->load(std::memory_order_acquire) ||
      !state->retry.ShouldRetry(ctx, state->attempt)) {
    CompleteAsync(state, reply);
    return;
  }

  const uint32_t next_attempt = ++state->attempt;
  const double backoff = state->retry.BackoffSeconds(next_attempt);
  if (backoff <= 0.0 || state->loop == nullptr) {
    StartAsyncAttempt(state);
    return;
  }

  state->loop->RunAfter(backoff, false,
                        [state]() { StartAsyncAttempt(state); });
}

void Client::CompleteAsync(
    const std::shared_ptr<AsyncRetryState>& state, const Reply& reply) {
  if (state->completed.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  if (state->callback) {
    InvokeAsyncCallbackSafely(state->callback, reply);
  }
}

void Client::InvokeAsync(const std::string& service, const std::string& method,
                         const std::string& request, AsyncCallback callback,
                         const CallOptions& options) {
  auto state = std::make_shared<AsyncRetryState>();
  state->loop = loop_;
  state->pool = pool_;
  state->metrics = metrics_;
  state->breaker = breaker_;
  state->enabled = async_enabled_;
  state->retry = options_.retry;
  state->service = service;
  state->method = method;
  state->request = request;
  state->callback = std::move(callback);
  state->options = options;
  StartAsyncAttempt(state);
}

Stub::Stub(Client* client, std::string service, std::string method)
    : client_(client),
      service_(std::move(service)),
      method_(std::move(method)) {}

Reply Stub::Call(const std::string& request,
                 const CallOptions& options) const {
  if (client_ == nullptr) {
    return Reply::Error(ErrorCode::kTransport, "rpc client unavailable");
  }
  return client_->InvokeSync(service_, method_, request,
                             client_->MergeOptions(options));
}

void Stub::AsyncCall(const std::string& request, AsyncCallback callback,
                     const CallOptions& options) const {
  if (client_ == nullptr) {
    InvokeAsyncCallbackSafely(
        callback,
        Reply::Error(ErrorCode::kTransport, "rpc client unavailable"));
    return;
  }
  client_->InvokeAsync(service_, method_, request, std::move(callback),
                       client_->MergeOptions(options));
}

}  // namespace rpc
}  // namespace zrpc
