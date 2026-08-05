// client.cc - Ping Pong echo client (adapted from muduo pingpong client)
// Usage: svc_game_3d_pingpong_client <ip> <port> <threads> <blocksize> <sessions> <time>

#include "zrpc/base/logger.h"
#include "zrpc/base/thread.h"
#include "zrpc/net/event_loop.h"
#include "zrpc/net/tcp_client.h"
#include "zrpc/net/tcp_connection.h"
#include "zrpc/net/socket.h"
#include "zrpc/base/buffer.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace zrpc;

// 全局统计
struct PingStats {
  std::atomic<int64_t> bytes_read{0};
  std::atomic<int64_t> msgs_read{0};
};
PingStats g_ping;

class Session {
 public:
  Session(EventLoop* loop, const std::string& ip, int port,
          const std::string& message)
      : client_(loop, ip, port, nullptr),
        message_(message) {
    client_.SetConnectionCallback(
        [this](const std::shared_ptr<TcpConnection>& conn) { OnConnection(conn); });
    client_.SetMessageCallback(
        [this](const std::shared_ptr<TcpConnection>& conn, Buffer* buf) {
          g_ping.msgs_read++;
          g_ping.bytes_read += buf->ReadableBytes();
          conn->Send(buf);
        });
  }

  void Start() { client_.Connect(); }
  void Stop()  { if (conn_) conn_->Shutdown(); }

 private:
  void OnConnection(const std::shared_ptr<TcpConnection>& conn) {
    if (conn->Connected()) {
      conn_ = conn;
      socket::SetTcpNoDelay(conn->GetSockfd(), true);
      conn->Send(message_);
    }
  }

  TcpClient                       client_;
  std::shared_ptr<TcpConnection>  conn_;
  std::string                     message_;
};

class EventLoopThread {
 public:
  EventLoop* Start() {
    thread_ = std::make_unique<Thread>();
    loop_ = thread_->StartLoop();
    return loop_;
  }

  // 析构时 ~Thread() 内部调 StopLoop() 停 loop + join 线程

 private:
  std::unique_ptr<Thread> thread_;
  EventLoop* loop_ = nullptr;
};

int main(int argc, char* argv[]) {
  if (argc != 7) {
    fprintf(stderr, "Usage: %s <ip> <port> <threads> <blocksize> <sessions> <time>\n", argv[0]);
    return 1;
  }

  const char* ip           = argv[1];
  uint16_t    port         = static_cast<uint16_t>(atoi(argv[2]));
  int         threadCount  = atoi(argv[3]);
  int         blockSize    = atoi(argv[4]);
  int         sessionCount = atoi(argv[5]);
  int         timeout      = atoi(argv[6]);

  if (threadCount <= 0) threadCount = 1;

  LOG_INFO << "pid = " << getpid() << ", starting pingpong client "
           << ip << ":" << port
           << " threads=" << threadCount
           << " blocksize=" << blockSize
           << " sessions=" << sessionCount
           << " timeout=" << timeout;

  // 构建消息块
  std::string message;
  for (int i = 0; i < blockSize; ++i) {
    message.push_back(static_cast<char>(i % 128));
  }

  // 启动线程池
  std::vector<std::shared_ptr<EventLoopThread>> threads;
  std::vector<EventLoop*> loops;
  for (int i = 0; i < threadCount; ++i) {
    auto t = std::make_shared<EventLoopThread>();
    loops.push_back(t->Start());
    threads.push_back(t);
  }

  // 轮询分配 session 到各线程
  std::vector<std::shared_ptr<Session>> sessions;
  for (int i = 0; i < sessionCount; ++i) {
    EventLoop* loop = loops[i % threadCount];
    auto s = std::make_shared<Session>(loop, ip, port, message);
    s->Start();
    sessions.push_back(s);
  }

  // 运行指定时长
  LOG_INFO << "running for " << timeout << " seconds...";
  std::this_thread::sleep_for(std::chrono::seconds(timeout));

  // 先收集统计（sessions / threads 还存活）
  int64_t totalBytes = g_ping.bytes_read.load();
  int64_t totalMsgs  = g_ping.msgs_read.load();
  LOG_WARN << "timeout, stopping all sessions";
  LOG_WARN << totalBytes << " total bytes read";
  LOG_WARN << totalMsgs  << " total messages read";
  if (totalMsgs > 0) {
    LOG_WARN << static_cast<double>(totalBytes) / totalMsgs
             << " average message size";
  }
  LOG_WARN << static_cast<double>(totalBytes) / (timeout * 1024 * 1024)
           << " MiB/s throughput";

  // 1) 通知所有连接关闭（不阻塞，只发 Shutdown 信号）
  for (auto& s : sessions) s->Stop();

  // 2) 等 EventLoop 处理完关闭事件
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // 3) 先析构 sessions（此时 fd 已关闭，TcpClient 析构安全）
  //    再析构 threads（~Thread() 内部 StopLoop + join）
  sessions.clear();
  threads.clear();

  return 0;
}
