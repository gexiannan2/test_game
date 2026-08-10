// server.cc - Ping Pong echo server (adapted from muduo pingpong server)
// Usage: svc_game_3d_pingpong_server <ip> <port> <threads>

#include "zrpc/base/logger.h"
#include "zrpc/net/event_loop.h"
#include "zrpc/net/tcp_server.h"
#include "zrpc/net/tcp_connection.h"
#include "zrpc/net/socket.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

using namespace zrpc;

int main(int argc, char* argv[]) {
  if (argc < 4) {
    fprintf(stderr, "Usage: %s <ip> <port> <threads>\n", argv[0]);
    return 1;
  }

  const char* ip         = argv[1];
  uint16_t    port       = static_cast<uint16_t>(atoi(argv[2]));
  int         threadCount = atoi(argv[3]);

  LOG_INFO << "pid = " << getpid() << ", starting pingpong server " << ip << ":" << port;

  EventLoop loop;

  TcpServer server(&loop, ip, port, std::any{});

  server.SetConnectionCallback(
      [](const std::shared_ptr<TcpConnection>& conn) {
        if (conn->Connected()) {
          socket::SetTcpNoDelay(conn->GetSockfd(), true);
        }
      });

  server.SetMessageCallback(
      [](const std::shared_ptr<TcpConnection>& conn, Buffer* buf) {
        conn->Send(buf);
      });

  if (threadCount > 1) {
    server.SetThreadNum(threadCount);
  }

  server.Start();
  loop.Run();

  return 0;
}
