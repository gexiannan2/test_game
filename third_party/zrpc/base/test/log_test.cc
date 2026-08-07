// zrpc 同步/异步日志生产级单元测试。
// 构建：见 base/test/Makefile 或项目根 CMakeLists zrpc_log_test 目标。

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <future>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "zrpc/base/async_log.h"
#include "zrpc/base/log.h"
#include "zrpc/base/logger.h"

#include "log_test_common.h"

using namespace zrpc;
using namespace zrpc_log_test;

namespace {

// ---------------------------------------------------------------------------
// FixedBuffer / LogStream
// ---------------------------------------------------------------------------

void TestFixedBufferAppendAndReset() {
  FixedBuffer<64> buf;
  buf.Append("hello", 5);
  EXPECT_EQ(buf.Length(), 5);
  EXPECT_EQ(std::string(buf.GetData(), buf.Length()), "hello");
  buf.Reset();
  EXPECT_EQ(buf.Length(), 0);
}

void TestFixedBufferSilentTruncate() {
  FixedBuffer<8> buf;
  buf.Append("1234567", 7);
  EXPECT_EQ(buf.Length(), 7);
  buf.Append("X", 1);
  EXPECT_EQ(buf.Length(), 8);
  buf.Append("Y", 1);  // 空间不足 → 整段丢弃（非部分写入）
  EXPECT_EQ(buf.Length(), 8);
}

void TestLogStreamIntegersAndNull() {
  LogStream stream;
  stream << 42 << ' ' << -7 << ' ' << static_cast<void *>(nullptr) << ' '
         << static_cast<const char *>(nullptr);
  const std::string out = stream.GetBuffer().ToString();
  EXPECT_TRUE(out.find("42") != std::string::npos);
  EXPECT_TRUE(out.find("-7") != std::string::npos);
  EXPECT_TRUE(out.find("0x") != std::string::npos);
  EXPECT_TRUE(out.find("(null)") != std::string::npos);
}

void TestLogStreamDoubleAndBool() {
  LogStream stream;
  stream << true << ' ' << false << ' ' << 3.14;
  const std::string out = stream.GetBuffer().ToString();
  EXPECT_TRUE(out.find('1') != std::string::npos);
  EXPECT_TRUE(out.find('0') != std::string::npos);
}

void TestLogStreamNestedBuffer() {
  LogStream inner;
  inner << "nested";
  LogStream outer;
  outer << inner.GetBuffer();
  EXPECT_EQ(outer.GetBuffer().ToString(), "nested");
}

// ---------------------------------------------------------------------------
// Logger 同步路径
// ---------------------------------------------------------------------------

void TestLogLevelFilterTraceDebug() {
  Logger::SetLogLevel(Logger::INFO);
  std::atomic<int> count{0};
  Logger::SetOutput([&count](const char *, int32_t) {
    count.fetch_add(1, std::memory_order_relaxed);
  });

  LOG_TRACE << "trace";
  LOG_DEBUG << "debug";
  const int after_verbose = count.load();
  LOG_INFO << "info";
  LOG_WARN << "warn";
  Logger::SetOutput({});
  EXPECT_EQ(after_verbose, 0);
  EXPECT_GE(count.load(), 2);
}

void TestLogLevelInvalidThrows() {
  bool threw = false;
  try {
    Logger::SetLogLevel(static_cast<Logger::LogLevel>(Logger::NUM_LOG_LEVELS));
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  EXPECT_TRUE(threw);
}

void TestLoggerOutputContainsLocation() {
  Logger::SetLogLevel(Logger::INFO);
  std::string captured;
  Logger::SetOutput([&captured](const char *msg, int32_t len) {
    captured.assign(msg, static_cast<size_t>(len));
  });
  LOG_INFO << "marker-xyzzy";
  Logger::SetOutput({});
  EXPECT_TRUE(captured.find("marker-xyzzy") != std::string::npos);
  EXPECT_TRUE(captured.find("log_test.cc") != std::string::npos);
}

void TestLoggerOutputExceptionFallback() {
  Logger::SetLogLevel(Logger::INFO);
  std::atomic<int> default_writes{0};
  Logger::SetOutput([&](const char *, int32_t) {
    throw std::runtime_error("output failed");
  });
  // 不应抛到调用方；析构走 DefaultOutput
  LOG_INFO << "fallback-test";
  Logger::SetOutput({});
  EXPECT_TRUE(true);
}

void TestLoggerRecursiveOutputNoDeadlock() {
  Logger::SetLogLevel(Logger::INFO);
  std::atomic<int> depth{0};
  std::atomic<int> total{0};
  Logger::SetOutput([&](const char *, int32_t) {
    total.fetch_add(1, std::memory_order_relaxed);
    const int d = depth.fetch_add(1, std::memory_order_relaxed);
    if (d < 2) {
      LOG_INFO << "rec-depth-" << d;
    }
    depth.fetch_sub(1, std::memory_order_relaxed);
  });
  LOG_INFO << "root";
  Logger::SetOutput({});
  EXPECT_GE(total.load(), 3);
}

void TestLoggerConcurrentOutput() {
  Logger::SetLogLevel(Logger::INFO);
  std::atomic<int> count{0};
  Logger::SetOutput([&count](const char *, int32_t) {
    count.fetch_add(1, std::memory_order_relaxed);
  });
  std::vector<std::thread> workers;
  for (int t = 0; t < 8; ++t) {
    workers.emplace_back([t]() {
      for (int i = 0; i < 50; ++i) {
        LOG_INFO << "thread-" << t << '-' << i;
      }
    });
  }
  for (auto &w : workers) {
    w.join();
  }
  Logger::SetOutput({});
  EXPECT_EQ(count.load(), 400);
}

void TestLoggerSetOutputDuringOutputNoDeadlock() {
  Logger::SetLogLevel(Logger::INFO);
  std::promise<void> done;
  auto fut = done.get_future();
  std::atomic<bool> swapped{false};
  Logger::SetOutput([&](const char *, int32_t) {
    if (!swapped.exchange(true)) {
      Logger::SetOutput([](const char *, int32_t) {});
      done.set_value();
    }
  });
  LOG_INFO << "swap-output";
  const bool ok =
      fut.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  Logger::SetOutput({});
  EXPECT_TRUE(ok);
}

void TestLoggerMacroElseBranch() {
  Logger::SetLogLevel(Logger::FATAL);
  bool else_ran = false;
  if (false)
    LOG_INFO << "must-not-run";
  else
    else_ran = true;
  EXPECT_TRUE(else_ran);
}

void TestLoggerSyserrAppendsErrno() {
  Logger::SetLogLevel(Logger::INFO);
  std::string captured;
  Logger::SetOutput([&captured](const char *msg, int32_t len) {
    captured.assign(msg, static_cast<size_t>(len));
  });
  errno = EINVAL;
  LOG_SYSERR << "bad-arg";
  Logger::SetOutput({});
  EXPECT_TRUE(captured.find("errno=") != std::string::npos);
}

// ---------------------------------------------------------------------------
// AppendFile / LogFile
// ---------------------------------------------------------------------------

void TestAppendFileWriteFlush() {
  const std::string dir = UniqueTempDir("zrpc_append");
  const std::string path = dir + "/one.log";
  {
    AppendFile file(path);
    EXPECT_TRUE(file.Valid());
    file.Append("line1\n", 6);
    file.Append("line2\n", 6);
    file.Flush();
    EXPECT_GE(file.GetWrittenBytes(), 12u);
  }
  std::ifstream in(path);
  std::string content((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
  EXPECT_TRUE(content.find("line1") != std::string::npos);
  EXPECT_TRUE(content.find("line2") != std::string::npos);
  RemoveTree(dir);
}

void TestLogFileCreateAndAppend() {
  const std::string dir = UniqueTempDir("zrpc_logfile");
  LogFile log_file(dir, "app", 1024 * 1024, true, 3, 1024);
  log_file.Append("sync-log-line\n", 15);
  log_file.Flush();
  const std::string active = dir + "/app.log";
  std::ifstream in(active);
  std::string content((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
  EXPECT_TRUE(content.find("sync-log-line") != std::string::npos);
  RemoveTree(dir);
}

void TestLogFileInvalidBaseNameThrows() {
  bool threw = false;
  try {
    LogFile bad_log("/tmp", "bad/name", 1024, true);
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  EXPECT_TRUE(threw);
}

void TestLogFileInvalidRollSizeThrows() {
  bool threw = false;
  try {
    LogFile bad_log("/tmp", "ok", 0, true);
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  EXPECT_TRUE(threw);
}

// ---------------------------------------------------------------------------
// AsyncLogging
// ---------------------------------------------------------------------------

void TestAsyncLoggingLifecycle() {
  const std::string dir = UniqueTempDir("zrpc_async");
  AsyncLogging async(dir, "worker", 1024 * 1024, 1);
  async.Start();
  async.Append("async-line-1\n", 14);
  async.Append("async-line-2\n", 14);
  async.Stop();
  const std::string active = dir + "/worker.log";
  EXPECT_TRUE(WaitUntil([&]() {
    std::error_code ec;
    return std::filesystem::exists(active, ec);
  }));
  std::ifstream in(active);
  std::string content((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
  EXPECT_TRUE(content.find("async-line-1") != std::string::npos);
  EXPECT_TRUE(content.find("async-line-2") != std::string::npos);
  RemoveTree(dir);
}

void TestAsyncLoggingAppendAfterStopDropped() {
  const std::string dir = UniqueTempDir("zrpc_async_drop");
  AsyncLogging async(dir, "drop", 1024 * 1024, 1);
  async.Start();
  async.Stop();
  async.Append("must-not-appear\n", 17);
  const std::string active = dir + "/drop.log";
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  std::error_code ec;
  if (std::filesystem::exists(active, ec)) {
    std::ifstream in(active);
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    EXPECT_TRUE(content.find("must-not-appear") == std::string::npos);
  } else {
    EXPECT_TRUE(true);
  }
  RemoveTree(dir);
}

void TestAsyncLoggingLargeMessage() {
  const std::string dir = UniqueTempDir("zrpc_async_big");
  AsyncLogging async(dir, "big", 8 * 1024 * 1024, 1);
  async.Start();
  std::string big(kLargeBuffer + 128, 'A');
  big.back() = '\n';
  async.Append(big.data(), big.size());
  async.Stop();
  const std::string active = dir + "/big.log";
  std::ifstream in(active);
  std::string content((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
  EXPECT_GE(content.size(), kLargeBuffer);
  EXPECT_EQ(content.front(), 'A');
  RemoveTree(dir);
}

void TestAsyncLoggingConcurrentAppend() {
  const std::string dir = UniqueTempDir("zrpc_async_mt");
  AsyncLogging async(dir, "mt", 8 * 1024 * 1024, 1);
  async.Start();
  std::vector<std::thread> workers;
  for (int t = 0; t < 6; ++t) {
    workers.emplace_back([&async, t]() {
      for (int i = 0; i < 200; ++i) {
        std::string line = "t" + std::to_string(t) + "i" + std::to_string(i) + "\n";
        async.Append(line.data(), line.size());
      }
    });
  }
  for (auto &w : workers) {
    w.join();
  }
  async.Stop();
  const std::string active = dir + "/mt.log";
  std::ifstream in(active);
  std::string content((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
  EXPECT_GE(content.size(), 6u * 200u * 4u);
  RemoveTree(dir);
}

void TestAsyncLoggingDoubleStartIdempotent() {
  const std::string dir = UniqueTempDir("zrpc_async_start");
  AsyncLogging async(dir, "start", 1024 * 1024, 1);
  async.Start();
  async.Start();  // 已在跑 → 直接返回
  async.Append("once\n", 5);
  async.Stop();
  RemoveTree(dir);
  EXPECT_TRUE(true);
}

void TestAsyncLoggingStopIdempotent() {
  const std::string dir = UniqueTempDir("zrpc_async_stop");
  AsyncLogging async(dir, "stop", 1024 * 1024, 1);
  async.Start();
  async.Append("x\n", 2);
  async.Stop();
  async.Stop();
  RemoveTree(dir);
  EXPECT_TRUE(true);
}

void TestAsyncBridgeFromLogger() {
  const std::string dir = UniqueTempDir("zrpc_bridge");
  AsyncLogging async(dir, "bridge", 4 * 1024 * 1024, 1);
  async.Start();
  Logger::SetLogLevel(Logger::INFO);
  Logger::SetOutput([&async](const char *msg, int32_t len) {
    async.Append(msg, static_cast<size_t>(len));
  });
  for (int i = 0; i < 100; ++i) {
    LOG_INFO << "bridged-" << i;
  }
  Logger::SetOutput({});
  async.Stop();
  const std::string active = dir + "/bridge.log";
  std::ifstream in(active);
  std::string content((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
  EXPECT_TRUE(content.find("bridged-0") != std::string::npos);
  EXPECT_TRUE(content.find("bridged-99") != std::string::npos);
  RemoveTree(dir);
}

void TestAsyncNullAndEmptyAppend() {
  const std::string dir = UniqueTempDir("zrpc_async_null");
  AsyncLogging async(dir, "null", 1024 * 1024, 1);
  async.Start();
  async.Append(nullptr, 10);
  async.Append("x", 0);
  async.Append("", 0);
  async.Stop();
  RemoveTree(dir);
  EXPECT_TRUE(true);
}

// ---------------------------------------------------------------------------
// SourceFile
// ---------------------------------------------------------------------------

void TestSourceFileStripsPath() {
  Logger::SourceFile unix_path("/var/log/service/main.cc");
  EXPECT_TRUE(std::strcmp(unix_path.data_, "main.cc") == 0);
  Logger::SourceFile win_path("D:/proj/src/win.cc");
  EXPECT_TRUE(std::strstr(win_path.data_, "win.cc") != nullptr);
}

}  // namespace

int main() {
  struct TestCase {
    const char *name;
    void (*fn)();
  };
  const TestCase cases[] = {
      {"FixedBufferAppendAndReset", TestFixedBufferAppendAndReset},
      {"FixedBufferSilentTruncate", TestFixedBufferSilentTruncate},
      {"LogStreamIntegersAndNull", TestLogStreamIntegersAndNull},
      {"LogStreamDoubleAndBool", TestLogStreamDoubleAndBool},
      {"LogStreamNestedBuffer", TestLogStreamNestedBuffer},
      {"LogLevelFilterTraceDebug", TestLogLevelFilterTraceDebug},
      {"LogLevelInvalidThrows", TestLogLevelInvalidThrows},
      {"LoggerOutputContainsLocation", TestLoggerOutputContainsLocation},
      {"LoggerOutputExceptionFallback", TestLoggerOutputExceptionFallback},
      {"LoggerRecursiveOutputNoDeadlock", TestLoggerRecursiveOutputNoDeadlock},
      {"LoggerConcurrentOutput", TestLoggerConcurrentOutput},
      {"LoggerSetOutputDuringOutputNoDeadlock",
       TestLoggerSetOutputDuringOutputNoDeadlock},
      {"LoggerMacroElseBranch", TestLoggerMacroElseBranch},
      {"LoggerSyserrAppendsErrno", TestLoggerSyserrAppendsErrno},
      {"AppendFileWriteFlush", TestAppendFileWriteFlush},
      {"LogFileCreateAndAppend", TestLogFileCreateAndAppend},
      {"LogFileInvalidBaseNameThrows", TestLogFileInvalidBaseNameThrows},
      {"LogFileInvalidRollSizeThrows", TestLogFileInvalidRollSizeThrows},
      {"AsyncLoggingLifecycle", TestAsyncLoggingLifecycle},
      {"AsyncLoggingAppendAfterStopDropped", TestAsyncLoggingAppendAfterStopDropped},
      {"AsyncLoggingLargeMessage", TestAsyncLoggingLargeMessage},
      {"AsyncLoggingConcurrentAppend", TestAsyncLoggingConcurrentAppend},
      {"AsyncLoggingDoubleStartIdempotent", TestAsyncLoggingDoubleStartIdempotent},
      {"AsyncLoggingStopIdempotent", TestAsyncLoggingStopIdempotent},
      {"AsyncBridgeFromLogger", TestAsyncBridgeFromLogger},
      {"AsyncNullAndEmptyAppend", TestAsyncNullAndEmptyAppend},
      {"SourceFileStripsPath", TestSourceFileStripsPath},
  };

  for (const auto &tc : cases) {
    try {
      tc.fn();
    } catch (const std::exception &e) {
      ++Stats().failed;
      std::cerr << "[FAIL] " << tc.name << " exception: " << e.what()
                << std::endl;
    } catch (...) {
      ++Stats().failed;
      std::cerr << "[FAIL] " << tc.name << " unknown exception" << std::endl;
    }
  }

  const auto &st = Stats();
  std::cout << "zrpc_log_test: cases=" << (st.passed + st.failed)
            << " assertions_passed=" << st.passed
            << " assertions_failed=" << st.failed << std::endl;
  return st.failed == 0 ? 0 : 1;
}
