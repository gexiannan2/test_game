#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

#include "zrpc/base/buffer.h"
#include "zrpc/base/group.h"
#include "zrpc/base/logger.h"
#include "zrpc/base/thread.h"
#include "zrpc/net/event_loop.h"

namespace {

bool TestLoggerMacroAndCallbacks() {
  zrpc::Logger::SetLogLevel(zrpc::Logger::FATAL);

  bool else_executed = false;
  if (false)
    LOG_INFO << "该日志不应输出";
  else
    else_executed = true;
  if (!else_executed) {
    return false;
  }

  std::atomic<int> output_count{0};
  auto output = [&output_count](const char *, int32_t) {
    output_count.fetch_add(1, std::memory_order_relaxed);
  };
  zrpc::Logger::SetLogLevel(zrpc::Logger::INFO);
  zrpc::Logger::SetOutput(output);

  std::vector<std::thread> workers;
  for (int i = 0; i < 4; ++i) {
    workers.emplace_back([output, i]() {
      for (int n = 0; n < 100; ++n) {
        zrpc::Logger::SetOutput(output);
        LOG_INFO << "logger-race " << i << ' ' << n;
      }
    });
  }
  for (auto &worker : workers) {
    worker.join();
  }

  zrpc::Logger::SetOutput({});
  return output_count.load(std::memory_order_relaxed) == 400;
}

bool TestBufferBoundaries() {
  zrpc::Buffer buffer;
  try {
    buffer.Append(static_cast<const char *>(nullptr), 1);
    return false;
  } catch (const std::invalid_argument &) {
  }
  try {
    buffer.Prepend(nullptr, 1);
    return false;
  } catch (const std::invalid_argument &) {
  }
  try {
    buffer.Retrieve(-1);
    return false;
  } catch (const std::invalid_argument &) {
  }
  try {
    buffer.ReadFd(zrpc::kInvalidSocket, nullptr);
    return false;
  } catch (const std::invalid_argument &) {
  }
  buffer.Append(static_cast<const char *>(nullptr), 0);
  return buffer.ReadableBytes() == 0;
}

bool TestTimerValidationAndExceptionBoundary() {
  zrpc::EventLoop loop;
  if (loop.RunAfter(-1.0, false, []() {}) != nullptr) {
    return false;
  }
  if (loop.RunAfter(0.0, true, []() {}) != nullptr) {
    return false;
  }
  if (loop.RunAfter((std::numeric_limits<double>::max)(), false,
                    []() {}) != nullptr) {
    return false;
  }
  zrpc::TimerCallback empty_callback;
  if (loop.RunAfter(0.01, false, std::move(empty_callback)) != nullptr) {
    return false;
  }

  bool completed = false;
  loop.RunAfter(0.01, false, []() {
    throw std::runtime_error("预期的定时器回调异常");
  });
  loop.RunAt(zrpc::AddTime(zrpc::TimeStamp::Now(), 0.03), 0.0, false,
             [&]() {
               completed = true;
               loop.Quit();
             });
  loop.RunAfter(1.0, false, [&]() { loop.Quit(); });
  loop.Run();
  return completed;
}

bool TestThreadLifecycle() {
  {
    zrpc::Thread thread([](zrpc::EventLoop *) {
      throw std::runtime_error("预期的启动异常");
    });
    try {
      thread.StartLoop();
      return false;
    } catch (const std::runtime_error &) {
    }
  }

  {
    zrpc::Thread thread;
    zrpc::EventLoop *loop = thread.StartLoop();
    if (loop == nullptr) {
      return false;
    }
    thread.StopLoop();
    try {
      thread.StartLoop();
      return false;
    } catch (const std::logic_error &) {
    }
  }

  std::promise<void> stopped;
  std::future<void> stopped_future = stopped.get_future();
  {
    zrpc::Thread thread;
    zrpc::EventLoop *loop = thread.StartLoop();
    loop->RunInLoop([&thread, &stopped]() {
      thread.StopLoop();
      stopped.set_value();
    });
    if (stopped_future.wait_for(std::chrono::seconds(2)) !=
        std::future_status::ready) {
      return false;
    }
  }
  return true;
}

bool TestGroupValidationAndConcurrency() {
  Engine engine;
  try {
    engine.Root().Register(1, {});
    return false;
  } catch (const std::invalid_argument &) {
  }

  std::atomic<int> calls{0};
  engine.Root().Register(
      7, [&calls](Context &) {
        calls.fetch_add(1, std::memory_order_relaxed);
      });

  std::vector<std::thread> workers;
  for (int i = 0; i < 4; ++i) {
    workers.emplace_back([&engine, &calls, i]() {
      for (int n = 0; n < 200; ++n) {
        if (i == 0) {
          engine.Root().Register(
              7, [&calls](Context &) {
                calls.fetch_add(1, std::memory_order_relaxed);
              });
        } else {
          engine.Do(7, {});
        }
      }
    });
  }
  for (auto &worker : workers) {
    worker.join();
  }
  return calls.load(std::memory_order_relaxed) > 0;
}

}  // namespace

int main() {
  const bool buffer_ok = TestBufferBoundaries();
  const bool logger_ok = TestLoggerMacroAndCallbacks();
  const bool timer_ok = TestTimerValidationAndExceptionBoundary();
  const bool thread_ok = TestThreadLifecycle();
  const bool group_ok = TestGroupValidationAndConcurrency();
  std::cout << "base regression: buffer=" << buffer_ok
            << " logger=" << logger_ok
            << " timer=" << timer_ok << " thread=" << thread_ok
            << " group=" << group_ok << '\n';
  return buffer_ok && logger_ok && timer_ok && thread_ok && group_ok ? 0 : 1;
}
