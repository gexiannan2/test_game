#include "zrpc/http/http_server.h"

#include <exception>

#include "zrpc/base/buffer.h"
#include "zrpc/base/logger.h"
#include "zrpc/net/socket.h"
namespace zrpc {
HttpServer::HttpServer(const std::string &ip, uint16_t port)
    : server_(&loop_, ip, port, nullptr) {
  server_.SetConnectionCallback(
      std::bind(&HttpServer::OnConnection, this, std::placeholders::_1));
  server_.SetMessageCallback(std::bind(&HttpServer::OnMessage, this,
                                       std::placeholders::_1,
                                       std::placeholders::_2));
}

void HttpServer::SetMessageCallback(HttpCallBack callback) {
  http_callback_ = callback;
}

HttpServer::~HttpServer() {}

void HttpServer::HttpClientTimerCallback(const std::thread::id &threadId) {
  std::lock_guard<std::mutex> lock(buckets_mutex_);
  auto it = connection_buckets_.find(threadId);
  if (it == connection_buckets_.end()) {
    LOG_WARN << "HTTP idle bucket is missing for an event loop";
    return;
  }
  if (it->second.size() >= kIdleSecond) {
    it->second.pop_front();
  }
  it->second.push_back(Bucket());
}

void HttpServer::Run() { loop_.Run(); }

void HttpServer::Stop() {
  if (!started_.load(std::memory_order_acquire)) {
    stopped_.store(true, std::memory_order_release);
    loop_.Quit();
    return;
  }
  server_.Stop([this]() {
    started_.store(false, std::memory_order_release);
    stopped_.store(true, std::memory_order_release);
    loop_.Quit();
  });
}

bool HttpServer::Start() {
  if (started_.load(std::memory_order_acquire)) {
    return true;
  }
  if (stopped_.load(std::memory_order_acquire)) {
    LOG_WARN << "HTTP server cannot be restarted after it has stopped";
    return false;
  }
  if (!server_.Start()) {
    LOG_SYSERR << "HTTP server failed to start";
    return false;
  }
  started_.store(true, std::memory_order_release);

  auto pools = server_.GetThreadPool()->GetAllLoops();

  for (size_t i = 0; i < pools.size(); i++) {
    {
      std::lock_guard<std::mutex> lock(buckets_mutex_);
      auto result = connection_buckets_.try_emplace(
          pools[i]->GetThreadId(), WeakConnectionList());
      if (result.second) {
        result.first->second.resize(kIdleSecond);
      }
    }
    pools[i]->RunAfter(kTimer, true,
                       std::bind(&HttpServer::HttpClientTimerCallback, this,
                                 pools[i]->GetThreadId()));
  }
  return true;
}

void HttpServer::OnConnection(const std::shared_ptr<TcpConnection> &conn) {
  if (conn->Connected()) {
    const HttpContext::Limits limits;
    conn->SetBufferLimits(
        limits.max_start_line_bytes + limits.max_header_bytes +
            limits.max_body_bytes,
        kMaxOutputBuffer);
    std::shared_ptr<HttpContext> c(new HttpContext(limits));
    conn->SetContext(c);
    EntryPtr entry = std::make_shared<Entry>(conn);
    {
      std::lock_guard<std::mutex> lock(buckets_mutex_);
      auto result = connection_buckets_.try_emplace(
          std::this_thread::get_id(), WeakConnectionList());
      if (result.second) {
        result.first->second.resize(kIdleSecond);
      }
      result.first->second.back().insert(entry);
    }
    conn->SetContext1(WeakEntryPtr(entry));
  } else {
    conn->ResetContext1();
  }
}

void HttpServer::OnMessage(const std::shared_ptr<TcpConnection> &conn,
                           Buffer *buffer) {
  const auto *context_ptr =
      std::any_cast<std::shared_ptr<HttpContext>>(&conn->GetContext());
  if (context_ptr == nullptr || !*context_ptr) {
    conn->ForceClose();
    return;
  }
  const std::shared_ptr<HttpContext> &context = *context_ptr;
  if (!context->ParseRequest(buffer)) {
    LOG_WARN << "Reject malformed HTTP request: " << context->GetError();
    conn->Send("HTTP/1.1 400 Bad Request\r\n"
               "Content-Length: 0\r\n"
               "Connection: close\r\n\r\n");
    conn->Shutdown();
    return;
  }

  if (context->GotAll()) {
    HttpResponse response;
    if (http_callback_) {
      try {
        http_callback_(conn, context->GetRequest(), &response);
      } catch (const std::exception &e) {
        LOG_SYSERR << "HTTP handler exception: " << e.what();
        response = HttpResponse();
        response.SetStatusCode(HttpResponse::k500InternalServerError);
        response.SetStatusMessage("Internal Server Error");
      } catch (...) {
        LOG_SYSERR << "HTTP handler exception: unknown";
        response = HttpResponse();
        response.SetStatusCode(HttpResponse::k500InternalServerError);
        response.SetStatusMessage("Internal Server Error");
      }
    } else {
      response.SetStatusCode(HttpResponse::k404NotFound);
      response.SetStatusMessage("Not Found");
    }
    if (response.GetStatusCode() == HttpResponse::kUnknown) {
      response.SetStatusCode(HttpResponse::k200k);
      response.SetStatusMessage("OK");
    }
    response.SetSuppressBody(
        context->GetRequest().GetMethod() == HttpRequest::kHead);
    response.SetCloseConnection(true);
    Buffer buf;
    if (!response.AppendToBuffer(&buf)) {
      LOG_WARN << "HTTP response serialization rejected unsafe output";
      response = HttpResponse();
      response.SetStatusCode(HttpResponse::k500InternalServerError);
      response.SetStatusMessage("Internal Server Error");
      response.SetCloseConnection(true);
      response.AppendToBuffer(&buf);
    }
    conn->Send(&buf);
    conn->Shutdown();
    context->Reset();
  }
}
}  // namespace zrpc
