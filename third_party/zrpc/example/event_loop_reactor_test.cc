#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "zrpc/base/buffer.h"
#include "zrpc/net/event_loop.h"
#include "zrpc/net/tcp_client.h"
#include "zrpc/net/tcp_connection.h"
#include "zrpc/net/tcp_server.h"

#ifdef _WIN32
#include <winsock2.h>
#endif

namespace {
std::atomic<int> g_failed{0};
std::atomic<int> g_passed{0};

void Fail(const char* test, const std::string& reason) {
  ++g_failed;
  std::cerr << "[FAIL] " << test << ": " << reason << '\n';
  std::cerr.flush();
}

void Pass(const char* test) {
  ++g_passed;
  std::cout << "[PASS] " << test << '\n';
  std::cout.flush();
}

bool RunCase(const char* name, const std::function<bool()>& fn) {
  try {
    if (fn()) {
      Pass(name);
      return true;
    }
    Fail(name, "assertion failed");
  } catch (const std::exception& ex) {
    Fail(name, ex.what());
  } catch (...) {
    Fail(name, "unknown exception");
  }
  return false;
}

using Clock = std::chrono::steady_clock;

bool TestTimerRunAfter() {
  zrpc::EventLoop loop;
  bool fired = false;
  const auto t0 = Clock::now();

  loop.RunAfter(0.08, false, [&]() {
    fired = true;
    loop.Quit();
  });
  loop.Run();

  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
  return fired && elapsed >= 50 && elapsed < 1000;
}

bool TestTimerRepeat() {
  zrpc::EventLoop loop;
  int count = 0;

  loop.RunAfter(0.03, true, [&]() {
    ++count;
    if (count >= 5) {
      loop.Quit();
    }
  });
  loop.RunAfter(2.0, false, [&]() { loop.Quit(); });
  loop.Run();
  return count == 5;
}

bool TestTimerCancel() {
  zrpc::EventLoop loop;
  bool fired = false;

  auto timer = loop.RunAfter(0.2, false, [&]() { fired = true; });
  loop.CancelAfter(timer);
  loop.RunAfter(0.05, false, [&]() { loop.Quit(); });
  loop.Run();
  return !fired;
}

bool TestEarliestTimerWins() {
  zrpc::EventLoop loop;
  std::string order;

  loop.RunAfter(0.12, false, [&]() { order += 'B'; });
  loop.RunAfter(0.05, false, [&]() {
    order += 'A';
    loop.Quit();
  });
  loop.RunAfter(1.0, false, [&]() { loop.Quit(); });
  loop.Run();
  return order == "A";
}

bool TestCrossThreadQueueInLoop() {
  zrpc::EventLoop loop;
  bool ran = false;

  std::thread worker([&]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    loop.QueueInLoop([&]() {
      ran = true;
      loop.Quit();
    });
  });

  loop.Run();
  worker.join();
  return ran;
}

bool TestWakeupMetrics() {
  zrpc::EventLoop loop;
  const uint64_t before = loop.GetMetrics().wakeup_count;

  std::thread worker([&]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    loop.QueueInLoop([&]() { loop.Quit(); });
  });

  loop.Run();
  worker.join();

  const uint64_t after = loop.GetMetrics().wakeup_count;
  return after > before;
}

bool TestPollOnceDrivesTimer() {
  zrpc::EventLoop loop;
  bool fired = false;

  loop.RunAfter(0.05, false, [&]() { fired = true; });

  const auto deadline = Clock::now() + std::chrono::milliseconds(500);
  while (!fired && Clock::now() < deadline) {
    loop.PollOnce(100);
  }
  return fired;
}

bool TestRunInLoopFromOtherThread() {
  zrpc::EventLoop loop;
  bool ran = false;

  std::thread worker([&]() {
    loop.RunInLoop([&]() {
      ran = true;
      loop.Quit();
    });
  });

  loop.Run();
  worker.join();
  return ran;
}

void DefaultEcho(const std::shared_ptr<zrpc::TcpConnection>& conn, zrpc::Buffer* buf) {
  if (buf->ReadableBytes() == 0) {
    return;
  }
  conn->Send(buf->Peek(), static_cast<int>(buf->ReadableBytes()));
  buf->RetrieveAll();
}

bool TestTimerDuringTcpActivity() {
  constexpr uint16_t kPort = 19400;
  zrpc::EventLoop loop;
  zrpc::TcpServer server(&loop, "127.0.0.1", static_cast<int16_t>(kPort), nullptr);
  server.SetThreadNum(0);
  server.SetMessageCallback(DefaultEcho);
  server.Start();

  bool timer_fired = false;
  bool echo_ok = false;
  const std::string payload = "timer-with-tcp";

  loop.RunAfter(0.05, false, [&]() { timer_fired = true; });

  zrpc::TcpClient client(&loop, "127.0.0.1", static_cast<int16_t>(kPort), nullptr);
  auto shutdown = [&]() {
    client.Stop();
    loop.Quit();
  };

  client.SetConnectionCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn) {
    if (conn->Connected()) {
      conn->Send(payload.data(), static_cast<int>(payload.size()));
      return;
    }
    loop.QueueInLoop(shutdown);
  });

  client.SetMessageCallback([&](const std::shared_ptr<zrpc::TcpConnection>& conn,
                                zrpc::Buffer* buf) {
    if (buf->ReadableBytes() < static_cast<int32_t>(payload.size())) {
      return;
    }
    if (buf->ToStringView().substr(0, payload.size()) == payload) {
      echo_ok = timer_fired;
      conn->Shutdown();
      loop.QueueInLoop(shutdown);
    }
  });

  loop.RunAfter(0.06, false, [&]() { client.Connect(); });
  loop.RunAfter(5.0, false, [&]() { loop.QueueInLoop(shutdown); });
  loop.Run();
  return echo_ok;
}

bool TestBurstCrossThreadWakeup() {
  zrpc::EventLoop loop;
  std::atomic<int> done{0};
  constexpr int kTasks = 32;

  std::vector<std::thread> workers;
  workers.reserve(kTasks);
  for (int i = 0; i < kTasks; ++i) {
    workers.emplace_back([&]() {
      loop.QueueInLoop([&]() {
        if (++done == kTasks) {
          loop.Quit();
        }
      });
    });
  }

  loop.RunAfter(5.0, false, [&]() { loop.Quit(); });
  loop.Run();

  for (auto& t : workers) {
    t.join();
  }
  return done.load() == kTasks;
}

}  // namespace

int main(int argc, char* argv[]) {
#ifdef _WIN32
  WSADATA wsa_data{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
    return 1;
  }
#endif

  int repeat = 1;
  if (argc > 1) {
    repeat = std::max(1, std::atoi(argv[1]));
  }

  struct Case {
    const char* name;
    std::function<bool()> fn;
  };

  const std::vector<Case> cases = {
      {"timer_run_after", TestTimerRunAfter},
      {"timer_repeat", TestTimerRepeat},
      {"timer_cancel", TestTimerCancel},
      {"earliest_timer", TestEarliestTimerWins},
      {"cross_thread_queue_in_loop", TestCrossThreadQueueInLoop},
      {"wakeup_metrics", TestWakeupMetrics},
      {"poll_once_drives_timer", TestPollOnceDrivesTimer},
      {"run_in_loop_other_thread", TestRunInLoopFromOtherThread},
      {"timer_during_tcp", TestTimerDuringTcpActivity},
      {"burst_cross_thread_wakeup", TestBurstCrossThreadWakeup},
  };

  std::cout << "zrpc reactor tests (timer/wakeup)"
#ifdef _WIN32
            << " [IOCP + pipe-wakeup + GQCS-timer]"
#elif defined(__linux__)
            << " [epoll + eventfd + timerfd]"
#else
            << " [poll + pipe-wakeup + poll-timer]"
#endif
            << ", cases=" << cases.size() << '\n';
  std::cout.flush();

  for (int r = 0; r < repeat; ++r) {
    if (repeat > 1) {
      std::cout << "--- round " << (r + 1) << '/' << repeat << " ---\n";
      std::cout.flush();
    }
    for (const auto& c : cases) {
      RunCase(c.name, c.fn);
    }
  }

  std::cout << "summary: passed=" << g_passed.load() << " failed=" << g_failed.load()
            << '\n';
  std::cout.flush();

#ifdef _WIN32
  WSACleanup();
#endif
  return g_failed.load() == 0 ? 0 : 1;
}
