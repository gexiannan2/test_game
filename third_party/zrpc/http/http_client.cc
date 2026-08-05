#include "zrpc/http/http_client.h"

#include <exception>
#include <stdexcept>
#include <vector>

#include "zrpc/base/logger.h"
#include "zrpc/net/socket.h"
namespace zrpc {
namespace {
void InvokeInvalidEndpoint(
    HttpClient::HttpCallBack &callback,
    const std::shared_ptr<TcpConnection> &conn,
    const std::any &context) {
  if (!callback) {
    return;
  }
  HttpResponse response;
  response.SetStatusCode(HttpResponse::k502BadGateway);
  response.SetStatusMessage("Bad Gateway");
  response.SetCloseConnection(true);
  response.SetBody("invalid upstream endpoint");
  try {
    callback(nullptr, response, conn, context);
  } catch (const std::exception &e) {
    LOG_WARN << "HTTP client callback exception: " << e.what();
  } catch (...) {
    LOG_WARN << "HTTP client callback exception: unknown";
  }
}
}  // 匿名命名空间

HttpClient::HttpClient(EventLoop *loop, int32_t request_timeout_seconds)
    : loop_(loop),
      lifetime_state_(std::make_shared<LifetimeState>()),
      request_timeout_seconds_(request_timeout_seconds),
      index_(0) {
  if (loop_ == nullptr) {
    throw std::invalid_argument("HttpClient event loop must not be null");
  }
  if (request_timeout_seconds_ <= 0) {
    throw std::invalid_argument("HttpClient timeout must be positive");
  }
}

HttpClient::~HttpClient() {
  std::shared_ptr<LifetimeState> lifetime = lifetime_state_;
  if (lifetime) {
    std::lock_guard<std::recursive_mutex> guard(lifetime->mutex);
    lifetime->alive = false;
  }
  lifetime_state_.reset();
  std::vector<int64_t> pending;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    pending.reserve(http_callbacks_.size());
    for (const auto &item : http_callbacks_) {
      pending.push_back(item.first);
    }
  }
  for (int64_t index : pending) {
    CompleteError(index, nullptr, HttpResponse::k502BadGateway,
                  "HTTP client stopped");
  }
}

bool HttpClient::CompleteRequest(
    int64_t index, const std::shared_ptr<TcpConnection> &outbound_conn,
    HttpResponse response) {
  std::shared_ptr<Timer> timer;
  std::shared_ptr<TcpClient> client;
  HttpCallBack callback;
  std::weak_ptr<TcpConnection> peer_conn;
  std::any user_context;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    auto callback_it = http_callbacks_.find(index);
    if (callback_it == http_callbacks_.end()) {
      return false;
    }
    callback = std::move(callback_it->second);
    http_callbacks_.erase(callback_it);

    auto timer_it = timers_.find(index);
    if (timer_it != timers_.end()) {
      timer = std::move(timer_it->second);
      timers_.erase(timer_it);
    }
    auto client_it = tcp_clients_.find(index);
    if (client_it != tcp_clients_.end()) {
      client = std::move(client_it->second);
      tcp_clients_.erase(client_it);
    }
    auto peer_it = tcp_conns_.find(index);
    if (peer_it != tcp_conns_.end()) {
      peer_conn = peer_it->second;
      tcp_conns_.erase(peer_it);
    }
    auto context_it = any_callbacks_.find(index);
    if (context_it != any_callbacks_.end()) {
      user_context = std::move(context_it->second);
      any_callbacks_.erase(context_it);
    }
  }
  if (timer) {
    loop_->CancelAfter(timer);
  }
  if (client) {
    client->DisConnect();
    client->Stop();
  }
  client.reset();
  if (callback) {
    try {
      callback(outbound_conn, response, peer_conn, user_context);
    } catch (const std::exception &e) {
      LOG_SYSERR << "HTTP client callback exception: " << e.what();
    } catch (...) {
      LOG_SYSERR << "HTTP client callback exception: unknown";
    }
  }
  return true;
}

bool HttpClient::CompleteError(
    int64_t index, const std::shared_ptr<TcpConnection> &outbound_conn,
    HttpResponse::HttpStatusCode status, const std::string &message) {
  HttpResponse response;
  response.SetStatusCode(status);
  if (status == HttpResponse::k504GatewayTimeout) {
    response.SetStatusMessage("Gateway Timeout");
  } else {
    response.SetStatusMessage("Bad Gateway");
  }
  response.SetCloseConnection(true);
  response.SetContentType("text/plain");
  response.SetBody(message);
  return CompleteRequest(index, outbound_conn, std::move(response));
}

void HttpClient::BindOutboundClient(int64_t index,
                                    const std::shared_ptr<TcpClient> &client) {
  const std::weak_ptr<LifetimeState> lifetime = lifetime_state_;
  client->SetConnectionErrorCallBack([this, lifetime, index]() {
    const std::shared_ptr<LifetimeState> state = lifetime.lock();
    if (!state) {
      return;
    }
    std::lock_guard<std::recursive_mutex> state_guard(state->mutex);
    if (!state->alive) {
      return;
    }
    loop_->QueueInLoop([this, lifetime, index]() {
      const std::shared_ptr<LifetimeState> queued_state = lifetime.lock();
      if (!queued_state) {
        return;
      }
      std::lock_guard<std::recursive_mutex> guard(queued_state->mutex);
      if (queued_state->alive) {
        CompleteError(index, nullptr, HttpResponse::k502BadGateway,
                      "upstream connection failed");
      }
    });
  });
}

void HttpClient::HttpClientTimerCallback(const int64_t index) {
  std::shared_ptr<TcpClient> client;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    auto client_it = tcp_clients_.find(index);
    if (client_it != tcp_clients_.end()) {
      client = client_it->second;
    }
  }
  if (client) {
    client->Stop();
  }
  CompleteError(index, client ? client->GetConnection() : nullptr,
                HttpResponse::k504GatewayTimeout,
                "upstream request timed out");
}

void HttpClient::GetUrl(const char *ip, int16_t port, const std::string &url,
                        const std::string &host,
                        const std::shared_ptr<TcpConnection> &conn,
                        const std::any &context, HttpCallBack &&callback) {
  if (ip == nullptr || *ip == '\0' ||
      static_cast<uint16_t>(port) == 0) {
    InvokeInvalidEndpoint(callback, conn, context);
    return;
  }
  std::shared_ptr<HttpRequest> request(new HttpRequest());
  int64_t req_index = 0;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    req_index = ++index_;
  }
  request->SetIndex(req_index);
  const bool request_valid =
      request->SetQuery(url) && request->AddHeader("Host", host);
  request->SetMethod(HttpRequest::kGet);

  std::shared_ptr<TcpClient> client(new TcpClient(loop_, ip, port, request));
  client->CloseRetry();
  const std::weak_ptr<LifetimeState> lifetime = lifetime_state_;
  client->SetConnectionCallback(
      [this, lifetime](const std::shared_ptr<TcpConnection> &connection) {
        const std::shared_ptr<LifetimeState> state = lifetime.lock();
        if (!state) {
          return;
        }
        std::lock_guard<std::recursive_mutex> guard(state->mutex);
        if (state->alive) {
          OnConnection(connection);
        }
      });
  client->SetMessageCallback(
      [this, lifetime](const std::shared_ptr<TcpConnection> &connection,
                       Buffer *buffer) {
        const std::shared_ptr<LifetimeState> state = lifetime.lock();
        if (!state) {
          return;
        }
        std::lock_guard<std::recursive_mutex> guard(state->mutex);
        if (state->alive) {
          OnMessage(connection, buffer);
        }
      });
  BindOutboundClient(req_index, client);

  bool timer_scheduled = false;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    tcp_clients_[req_index] = client;
    tcp_conns_[req_index] = conn;
    http_callbacks_[req_index] = std::move(callback);
    any_callbacks_[req_index] = context;
    timers_[req_index] =
        loop_->RunAfter(request_timeout_seconds_, false,
                        [this, lifetime, req_index]() {
          const std::shared_ptr<LifetimeState> state = lifetime.lock();
          if (!state) {
            return;
          }
          std::lock_guard<std::recursive_mutex> guard(state->mutex);
          if (state->alive) {
            HttpClientTimerCallback(req_index);
          }
        });
    timer_scheduled = timers_[req_index] != nullptr;
  }
  if (!request_valid || !timer_scheduled) {
    CompleteError(req_index, nullptr, HttpResponse::k502BadGateway,
                  "invalid outbound HTTP request");
    return;
  }
  try {
    client->Connect();
  } catch (const std::exception &e) {
    CompleteError(req_index, nullptr, HttpResponse::k502BadGateway,
                  std::string("upstream connect failed: ") + e.what());
  } catch (...) {
    CompleteError(req_index, nullptr, HttpResponse::k502BadGateway,
                  "upstream connect failed");
  }
}

void HttpClient::PostUrl(const char *ip, int16_t port, const std::string &url,
                         const std::string &body, const std::string &host,
                         const std::string &type,
                         const std::shared_ptr<TcpConnection> &conn,
                         const std::any &context, HttpCallBack &&callback) {
  if (ip == nullptr || *ip == '\0' ||
      static_cast<uint16_t>(port) == 0) {
    InvokeInvalidEndpoint(callback, conn, context);
    return;
  }
  std::shared_ptr<HttpRequest> request(new HttpRequest());
  int64_t req_index = 0;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    req_index = ++index_;
  }
  request->SetIndex(req_index);
  bool request_valid = request->SetQuery(url);
  request->SetBody(body);
  request->SetMethod(HttpRequest::kPost);
  request_valid = request->AddHeader("Host", host) && request_valid;
  request_valid = request->AddHeader("Content-Type", type) && request_valid;
  request_valid =
      request->AddHeader("Content-Length", std::to_string(body.size())) &&
      request_valid;

  std::shared_ptr<TcpClient> client(new TcpClient(loop_, ip, port, request));
  client->CloseRetry();
  const std::weak_ptr<LifetimeState> lifetime = lifetime_state_;
  client->SetConnectionCallback(
      [this, lifetime](const std::shared_ptr<TcpConnection> &connection) {
        const std::shared_ptr<LifetimeState> state = lifetime.lock();
        if (!state) {
          return;
        }
        std::lock_guard<std::recursive_mutex> guard(state->mutex);
        if (state->alive) {
          OnConnection(connection);
        }
      });
  client->SetMessageCallback(
      [this, lifetime](const std::shared_ptr<TcpConnection> &connection,
                       Buffer *buffer) {
        const std::shared_ptr<LifetimeState> state = lifetime.lock();
        if (!state) {
          return;
        }
        std::lock_guard<std::recursive_mutex> guard(state->mutex);
        if (state->alive) {
          OnMessage(connection, buffer);
        }
      });
  BindOutboundClient(req_index, client);

  bool timer_scheduled = false;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    tcp_clients_[req_index] = client;
    tcp_conns_[req_index] = conn;
    http_callbacks_[req_index] = std::move(callback);
    any_callbacks_[req_index] = context;
    timers_[req_index] =
        loop_->RunAfter(request_timeout_seconds_, false,
                        [this, lifetime, req_index]() {
          const std::shared_ptr<LifetimeState> state = lifetime.lock();
          if (!state) {
            return;
          }
          std::lock_guard<std::recursive_mutex> guard(state->mutex);
          if (state->alive) {
            HttpClientTimerCallback(req_index);
          }
        });
    timer_scheduled = timers_[req_index] != nullptr;
  }
  if (!request_valid || !timer_scheduled) {
    CompleteError(req_index, nullptr, HttpResponse::k502BadGateway,
                  "invalid outbound HTTP request");
    return;
  }
  try {
    client->Connect();
  } catch (const std::exception &e) {
    CompleteError(req_index, nullptr, HttpResponse::k502BadGateway,
                  std::string("upstream connect failed: ") + e.what());
  } catch (...) {
    CompleteError(req_index, nullptr, HttpResponse::k502BadGateway,
                  "upstream connect failed");
  }
}

void HttpClient::OnConnection(const std::shared_ptr<TcpConnection> &conn) {
  const auto *request_ptr =
      std::any_cast<std::shared_ptr<HttpRequest>>(&conn->GetContext());
  if (request_ptr == nullptr || !*request_ptr) {
    conn->ForceClose();
    return;
  }
  const std::shared_ptr<HttpRequest> &request = *request_ptr;
  if (conn->Connected()) {
    if (!request->AppendToBuffer(conn->OutputBuffer())) {
      CompleteError(request->GetIndex(), conn, HttpResponse::k502BadGateway,
                    "outbound HTTP serialization failed");
      conn->ForceClose();
      return;
    }
    conn->SendPipe();

    std::shared_ptr<HttpContext> c(new HttpContext());
    c->SetResponseToHeadRequest(
        request->GetMethod() == HttpRequest::kHead);
    conn->SetContext1(c);
  } else {
    const auto *context_ptr =
        std::any_cast<std::shared_ptr<HttpContext>>(&conn->GetContext1());
    if (context_ptr != nullptr && *context_ptr) {
      Buffer empty;
      if ((*context_ptr)->ParseResponseEof(&empty) &&
          (*context_ptr)->GotAll()) {
        CompleteRequest(request->GetIndex(), conn,
                        (*context_ptr)->GetResponse());
        return;
      }
    }
    CompleteError(request->GetIndex(), conn, HttpResponse::k502BadGateway,
                  "upstream disconnected before a complete response");
  }
}

void HttpClient::OnMessage(const std::shared_ptr<TcpConnection> &conn,
                           Buffer *buffer) {
  const auto *context_ptr =
      std::any_cast<std::shared_ptr<HttpContext>>(&conn->GetContext1());
  const auto *request_ptr =
      std::any_cast<std::shared_ptr<HttpRequest>>(&conn->GetContext());
  if (context_ptr == nullptr || !*context_ptr || request_ptr == nullptr ||
      !*request_ptr) {
    conn->ForceClose();
    return;
  }
  const std::shared_ptr<HttpContext> &context = *context_ptr;
  const std::shared_ptr<HttpRequest> &request = *request_ptr;
  if (!context->ParseResponse(buffer)) {
    CompleteError(request->GetIndex(), conn, HttpResponse::k502BadGateway,
                  "invalid upstream HTTP response: " +
                      context->GetError());
    conn->ForceClose();
    return;
  }

  if (context->GotAll()) {
    CompleteRequest(request->GetIndex(), conn, context->GetResponse());
  }
}
}  // namespace zrpc
