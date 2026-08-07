// svc_game_3d_server 进程入口：解析监听地址，启动 GameServer 事件循环
//
// 用法：svc_game_3d_server [port] [ip] [-q|-v]
//   -q  安静模式(只 WARN 及以上)
//   -v  详细模式(全部 INFO)
//
// 环境变量：
//   GAME_LOG_LEVEL   INFO|WARN|ERROR|DEBUG  (默认 INFO)
//   GAME_LOG_DIR     日志目录               (默认 ./logs，自动创建)
//   GAME_LOG_STDOUT  0|false                (默认开启，=0 关闭控制台输出)
//
// 压测示例：
//   GAME_LOG_LEVEL=WARN ulimit -n 65535 ./bin/svc_game_3d_server

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

#include "common/init.h"
#include "game_server.h"
#include "server_defaults.h"
#include "zrpc/base/async_log.h"
#include "zrpc/base/logger.h"

namespace {

// 信号 handler 只做原子置位（async-signal-safe），不触碰任何锁/IO。
// 实际 drain（连接清理/LeaveMap/停线程池/Quit）由 GameServer 在 loop
// 线程的 world_tick 轮询标志后执行，规避跨线程调用 RunInLoop 的死锁风险。
void OnTermSignal(int sig) {
    (void)sig;
    RequestStop();
}

// 安装信号处理：SIGPIPE 忽略，SIGINT/SIGTERM 优雅退出
void InitServerSignals() {
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, OnTermSignal);
    std::signal(SIGTERM, OnTermSignal);
}

// 异步日志：封装等级解析 + 落盘初始化，返回 AsyncLogging 指针供退出时 Stop
std::shared_ptr<zrpc::AsyncLogging> InitAsyncLogging(int argc, char** argv,
                                                     std::string& out_level) {
    // 1) 日志等级：命令行 -q/-v > 环境变量 GAME_LOG_LEVEL > 默认 INFO
    bool force_quiet = false;
    bool force_verbose = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-q") == 0 || std::strcmp(argv[i], "--quiet") == 0) {
            force_quiet = true;
        } else if (std::strcmp(argv[i], "-v") == 0 || std::strcmp(argv[i], "--verbose") == 0) {
            force_verbose = true;
        }
    }
    out_level = "INFO";
    if (force_quiet) {
        zrpc::Logger::SetLogLevel(zrpc::Logger::WARN);
        out_level = "WARN";
    } else if (force_verbose) {
        zrpc::Logger::SetLogLevel(zrpc::Logger::INFO);
        out_level = "INFO";
    } else if (const char* lv = std::getenv("GAME_LOG_LEVEL")) {
        if (std::strcmp(lv, "WARN") == 0 || std::strcmp(lv, "warn") == 0) {
            zrpc::Logger::SetLogLevel(zrpc::Logger::WARN);
            out_level = "WARN";
        } else if (std::strcmp(lv, "ERROR") == 0 || std::strcmp(lv, "error") == 0) {
            zrpc::Logger::SetLogLevel(zrpc::Logger::ERR);
            out_level = "ERROR";
        } else if (std::strcmp(lv, "DEBUG") == 0 || std::strcmp(lv, "debug") == 0) {
            zrpc::Logger::SetLogLevel(zrpc::Logger::DEBUG);
            out_level = "DEBUG";
        } else {
            zrpc::Logger::SetLogLevel(zrpc::Logger::INFO);
        }
    } else {
        zrpc::Logger::SetLogLevel(zrpc::Logger::INFO);
    }

    // 2) 日志目录：环境变量 GAME_LOG_DIR > 默认 ./logs
    std::string log_dir = "./logs";
    if (const char* env_dir = std::getenv("GAME_LOG_DIR")) {
        if (env_dir[0] != '\0') log_dir = env_dir;
    }

    // 3) 启动 AsyncLogging：专用线程写盘，不阻塞游戏循环
    //    文件名: {log_dir}/svc_game_3d.YYYY-MM-DD-HH-MM-SS.log
    constexpr size_t kRollSize = 64 * 1024 * 1024;  // 64MB 滚动
    auto async_log = std::make_shared<zrpc::AsyncLogging>(
        log_dir,        // 目录(自动创建)
        "svc_game_3d",  // 文件名前缀
        kRollSize,      // 单文件最大 64MB
        3);             // 3秒 flush 一次
    async_log->Start();

    // 默认同时输出到控制台+文件；GAME_LOG_STDOUT=0 关闭控制台(生产压测用)
    const bool mirror_stdout = ([]() {
        const char* v = std::getenv("GAME_LOG_STDOUT");
        return !(v && (std::strcmp(v, "0") == 0 || std::strcmp(v, "false") == 0));
    })();
    if (mirror_stdout) {
        zrpc::Logger::SetOutput(
            [async_log](const char* msg, int32_t len) {
                async_log->Append(msg, static_cast<size_t>(len));
                ::fwrite(msg, 1, static_cast<size_t>(len), stdout);
            });
        zrpc::Logger::SetFlush([] { ::fflush(stdout); });
    } else {
        zrpc::Logger::SetOutput(
            [async_log](const char* msg, int32_t len) {
                async_log->Append(msg, static_cast<size_t>(len));
            });
        zrpc::Logger::SetFlush([] {});  // AsyncLogging 自己定期 flush
    }
    return async_log;
}

// 解析监听地址：argv[1]=port, argv[2]=ip（跳过 -q/-v）
void ParseListenAddr(int argc, char** argv, std::string& ip, int& port) {
    ip = server::kDefaultListenIp;
    port = server::kDefaultListenPort;
    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] == '-') {
            continue;
        }
        if (positional == 0) {
            port = std::stoi(argv[i]);
        } else if (positional == 1) {
            ip = argv[i];
        }
        ++positional;
    }
}

}  // namespace

int main(int argc, char** argv) {
    InitSignals();
    InitServerSignals();

    // 初始化异步日志（等级 + 落盘）
    std::string log_level;
    auto async_log = InitAsyncLogging(argc, argv, log_level);

    // 解析监听地址
    std::string ip;
    int port = 0;
    ParseListenAddr(argc, argv, ip, port);

    std::cout << "svc_game_3d_server listen " << ip << ":" << port
              << "  log_level=" << log_level << std::endl;

    // 启动服务器
    auto server = std::make_shared<GameServer>(ip, port);
    server->Start();
    if (!server->ListenOk()) {
        std::cerr << "FATAL: failed to listen on " << ip << ":" << port << std::endl;
        async_log->Stop();
        return 1;
    }
    server->Loop();

    // 退出清理：停异步日志线程，恢复默认输出(stdout)
    async_log->Stop();
    zrpc::Logger::SetOutput({});
    zrpc::Logger::SetFlush({});

    return 0;
}
