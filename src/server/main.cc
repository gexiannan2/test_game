// svc_game_3d_server 进程入口：解析监听地址，启动 GameServer 事件循环
//
// 用法：svc_game_3d_server [port] [ip]，默认 127.0.0.1:20002
// 压测：GAME_LOG_LEVEL=WARN ulimit -n 65535 ./bin/svc_game_3d_server

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "common/init.h"
#include "game_server.h"
#include "zrpc/base/logger.h"

namespace {
// 信号 handler 只做原子置位（async-signal-safe），不触碰任何锁/IO。
// 实际 drain（连接清理/LeaveMap/停线程池/Quit）由 GameServer 在 loop
// 线程的 world_tick 轮询标志后执行，规避跨线程调用 RunInLoop 的死锁风险。
void OnTermSignal(int sig) {
    (void)sig;
    RequestStop();
}
}  // namespace

int main(int argc, char** argv) {
  InitSignals();
  // 客户端断线 write 可能 SIGPIPE；部分库会重置信号，再设一次
  std::signal(SIGPIPE, SIG_IGN);
  // Ctrl+C / kill：置位停止标志，由事件循环 drain 后优雅退出
  std::signal(SIGINT, OnTermSignal);
  std::signal(SIGTERM, OnTermSignal);

  if (const char* lv = std::getenv("GAME_LOG_LEVEL")) {
    if (std::strcmp(lv, "WARN") == 0 || std::strcmp(lv, "warn") == 0) {
      zrpc::Logger::SetLogLevel(zrpc::Logger::WARN);
    } else if (std::strcmp(lv, "ERROR") == 0 || std::strcmp(lv, "error") == 0) {
      zrpc::Logger::SetLogLevel(zrpc::Logger::ERR);
    } else if (std::strcmp(lv, "INFO") == 0 || std::strcmp(lv, "info") == 0) {
      zrpc::Logger::SetLogLevel(zrpc::Logger::INFO);
    }
  }

  std::string ip = "10.23.0.99";
  int port = 20002;
  if (argc >= 2) port = std::stoi(argv[1]);
  if (argc >= 3) ip = argv[2];

  std::cout << "svc_game_3d_server listen " << ip << ":" << port << std::endl;

  auto server = std::make_shared<GameServer>(ip, port);
  server->Start();
  if (!server->ListenOk()) {
    std::cerr << "FATAL: failed to listen on " << ip << ":" << port << std::endl;
    return 1;
  }
  server->Loop();

  return 0;
}
