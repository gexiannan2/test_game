#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "zrpc/base/timer.h"
#include "zrpc/http/http_context.h"
#include "zrpc/net/tcp_client.h"
#include "zrpc/net/tcp_connection.h"
namespace zrpc {
class HttpClient {
 public:
  typedef std::function<void(
      const std::shared_ptr<TcpConnection> &, HttpResponse &,
      const std::weak_ptr<TcpConnection> &, const std::any &)>
      HttpCallBack;
  explicit HttpClient(EventLoop *loop, int32_t request_timeout_seconds = 60);
  ~HttpClient();

  void OnConnection(const std::shared_ptr<TcpConnection> &conn);

  void OnMessage(const std::shared_ptr<TcpConnection> &conn, Buffer *buffer);

  void PostUrl(const char *ip, int16_t port, const std::string &url,
               const std::string &body, const std::string &host,
               const std::string &type,
               const std::shared_ptr<TcpConnection> &conn,
               const std::any &context, HttpCallBack &&callback);

  void GetUrl(const char *ip, int16_t port, const std::string &url,
              const std::string &host,
              const std::shared_ptr<TcpConnection> &conn,
              const std::any &context, HttpCallBack &&callback);

  void HttpClientTimerCallback(const int64_t index);

 private:
  struct LifetimeState {
    std::recursive_mutex mutex;
    bool alive = true;
  };

  bool CompleteRequest(int64_t index,
                       const std::shared_ptr<TcpConnection> &outbound_conn,
                       HttpResponse response);
  bool CompleteError(int64_t index,
                     const std::shared_ptr<TcpConnection> &outbound_conn,
                     HttpResponse::HttpStatusCode status,
                     const std::string &message);
  void BindOutboundClient(int64_t index,
                          const std::shared_ptr<TcpClient> &client);

  EventLoop *loop_;
  std::shared_ptr<LifetimeState> lifetime_state_;
  std::mutex mutex_;
  std::unordered_map<int64_t, std::shared_ptr<Timer>> timers_;
  std::unordered_map<int64_t, std::shared_ptr<TcpClient>> tcp_clients_;
  std::unordered_map<int64_t, std::weak_ptr<TcpConnection>> tcp_conns_;
  std::unordered_map<int64_t, HttpCallBack> http_callbacks_;
  std::unordered_map<int64_t, std::any> any_callbacks_;
  int32_t request_timeout_seconds_;
  int64_t index_;
};
}  // namespace zrpc
