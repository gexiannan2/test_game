#pragma once

// 通用线程 id 工具：返回内核/系统层面的小整数线程 id，便于日志输出与系统工具对应。
//
// 为什么不用 std::this_thread::get_id()？
//   - std::thread::id 在 Linux/glibc + libstdc++ 下是 pthread_t（指向 TCB 的指针），
//     ostringstream 输出是 0x7xxxxxxxxxxxxx 量级的巨大数字，不直观。
//   - 本工具返回内核小整数 tid，可直接和 top -H / gdb info threads /
//     /proc/<pid>/task/<tid> / perf top -t <tid> 对应，排查问题更方便。
//
// 用途：日志输出、跨线程排查、运维定位。
// 若仅需线程相等性比较（如 IsInLoopThread），直接用 std::thread::id 即可，无需本工具。

#include <cstdint>

#if defined(__linux__)
    #include <sys/syscall.h>
    #include <unistd.h>
#elif defined(_WIN32)
    #include <windows.h>
#else
    #include <functional>
    #include <thread>
#endif

namespace e996
{
    // 返回当前线程的内核/系统小整数 id。
    //   - Linux:    syscall(SYS_gettid) 返回 pid_t（内核 tid）
    //   - Windows:  GetCurrentThreadId() 返回 DWORD
    //   - 其他:     退化为 std::thread::id 的 hash 值（保证同一线程稳定、不同线程不同）
    inline std::int64_t GetThreadId() noexcept
    {
#if defined(__linux__)
        return static_cast<std::int64_t>(::syscall(SYS_gettid));
#elif defined(_WIN32)
        return static_cast<std::int64_t>(::GetCurrentThreadId());
#else
        return static_cast<std::int64_t>(
            std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
    }
} // namespace e996
