#pragma once

#include <atomic>
#include <deque>
#include <mutex>
#include <unordered_set>

#include "zrpc/http/http_context.h"
#include "zrpc/net/tcp_connection.h"
#include "zrpc/net/tcp_server.h"

namespace zrpc {
class EventLoop;

class HttpServer {
 public:
  typedef std::function<void(const std::shared_ptr<TcpConnection> &,
                             const HttpRequest &, HttpResponse *response)>
      HttpCallBack;

  struct Entry {
    explicit Entry(const std::weak_ptr<TcpConnection> &weak_conn)
        : weak_conn_(weak_conn) {}

    ~Entry() {
      std::shared_ptr<TcpConnection> conn = weak_conn_.lock();
      if (conn) {
        conn->Shutdown();
      }
    }

    std::weak_ptr<TcpConnection> weak_conn_;
  };

  HttpServer(const std::string &ip, uint16_t port);

  ~HttpServer();

  void SetThreadNum(int num_thread) { server_.SetThreadNum(num_thread); }
  void SetMessageCallback(HttpCallBack callback);

  bool Start();
  void Stop();
  void Run();
  void HttpClientTimerCallback(const std::thread::id &threadId);
  void OnConnection(const std::shared_ptr<TcpConnection> &conn);
  void OnMessage(const std::shared_ptr<TcpConnection> &conn, Buffer *buffer);

  typedef std::shared_ptr<Entry> EntryPtr;
  typedef std::weak_ptr<Entry> WeakEntryPtr;
  typedef std::unordered_set<EntryPtr> Bucket;
  typedef std::deque<Bucket> WeakConnectionList;

 private:
  HttpServer(const HttpServer &);

  void operator=(const HttpServer &);

  EventLoop loop_;
  TcpServer server_;
  HttpCallBack http_callback_;
  std::mutex buckets_mutex_;
  std::unordered_map<std::thread::id, WeakConnectionList> connection_buckets_;
  std::atomic<bool> started_{false};
  std::atomic<bool> stopped_{false};
  const static int32_t kHeart = 10;
  const static int32_t kTimer = 1;
  const static int32_t kIdleSecond = 20;
  const static size_t kMaxOutputBuffer = 16 * 1024 * 1024;
};
}  // namespace zrpc
