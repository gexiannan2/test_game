#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace zrpc_log_test {

struct TestStats {
  int passed = 0;
  int failed = 0;
};

inline TestStats &Stats() {
  static TestStats s;
  return s;
}

inline void ExpectTrue(bool cond, const char *expr, const char *file, int line,
                     const char *name) {
  if (cond) {
    ++Stats().passed;
  } else {
    ++Stats().failed;
    std::cerr << "[FAIL] " << name << "  " << file << ':' << line << "  "
              << expr << std::endl;
  }
}

#define EXPECT_TRUE(expr) \
  ::zrpc_log_test::ExpectTrue((expr), #expr, __FILE__, __LINE__, __func__)

#define EXPECT_EQ(a, b) EXPECT_TRUE((a) == (b))
#define EXPECT_NE(a, b) EXPECT_TRUE((a) != (b))
#define EXPECT_GE(a, b) EXPECT_TRUE((a) >= (b))
#define EXPECT_LE(a, b) EXPECT_TRUE((a) <= (b))

inline std::string UniqueTempDir(const char *prefix) {
  const auto base = std::filesystem::temp_directory_path();
  for (int i = 0; i < 10000; ++i) {
    auto path = base / (std::string(prefix) + "_" + std::to_string(i));
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
      std::filesystem::create_directories(path, ec);
      return path.string();
    }
  }
  throw std::runtime_error("failed to allocate temp dir");
}

inline void RemoveTree(const std::string &path) {
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
}

inline bool WaitUntil(const std::function<bool()> &pred,
                      std::chrono::milliseconds timeout =
                          std::chrono::milliseconds(3000)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return pred();
}

}  // namespace zrpc_log_test
