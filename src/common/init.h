#pragma once

#include <atomic>
#include <csignal>

// 网络服务进程必要的信号处理初始化
// 调用时机：main() 函数第一行，WSAStartup 之前
inline void InitSignals()
{
#ifndef _WIN32
    // SIGPIPE: 对端关闭时 write 触发，必须忽略否则进程被杀
    // 注意：zrpc socket::Write 已加 MSG_NOSIGNAL，此处为兜底
    std::signal(SIGPIPE, SIG_IGN);

    // SIGHUP: 终端断开（nohup/守护进程场景），忽略避免被杀
    std::signal(SIGHUP, SIG_IGN);

    // SIGURG: TCP 带外数据，zrpc 未处理但内核可能发送，忽略避免默认终止
    std::signal(SIGURG, SIG_IGN);
#else
    (void)0;
#endif
}

// ---- 优雅停止标志 ----
// 信号 handler 只做 async-signal-safe 的原子置位（不调 RunInLoop/mutex，
// 避免信号打断持锁线程后自死锁）；业务线程在事件循环里轮询此标志触发
// 完整的 drain 流程（LeaveMap / ForceClose / 停线程池 / Quit）。
inline std::atomic<bool>& StopRequestedFlag() {
    static std::atomic<bool> flag{false};
    return flag;
}
inline void RequestStop() {
    StopRequestedFlag().store(true, std::memory_order_release);
}
inline bool IsStopRequested() {
    return StopRequestedFlag().load(std::memory_order_acquire);
}

