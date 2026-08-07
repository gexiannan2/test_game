// 轻量单元测试框架；GAME_TEST_FILTER、GAME_AOI_*、GAME_SCALE_ENTITY_COUNT 等环境变量。
#pragma once

#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "common/aoi_def.h"  // MoveStopReason

// MoveStopReason 打印支持（EXPECT_EQ 失败时需要，必须放在全局命名空间）
inline std::ostream& operator<<(std::ostream& os, MoveStopReason r) {
    return os << "MoveStopReason(" << static_cast<int>(r) << ")";
}

namespace test {

// 断言失败时携带的异常信息（套件名、用例名、详细消息）
struct TestFailure {
  std::string suite;
  std::string name;
  std::string message;
};

// 当前正在执行的用例名（通过静态变量供 Fail() 读取）
class TestScope {
 public:
  TestScope(const char* suite, const char* name) {
    suite_ = suite;
    name_ = name;
  }
  static const char* Suite() { return suite_; }
  static const char* Name() { return name_; }

 private:
  static inline const char* suite_ = "";
  static inline const char* name_ = "";
};

// GAME_TEST_FILTER 非空时，仅运行「套件.用例名」包含该子串的用例
inline bool ShouldRunCase(const std::string& suite, const std::string& name) {
  const char* filter = std::getenv("GAME_TEST_FILTER");
  if (filter == nullptr || *filter == '\0') return true;
  return (suite + "." + name).find(filter) != std::string::npos;
}

// 全局单例：收集并执行所有 GAME_TEST 注册的用例
class Runner {
 public:
  static Runner& Instance() {
    static Runner r;
    return r;
  }
  void Register(const std::string& suite, const std::string& name,
                std::function<void()> fn) {
    cases_.push_back({suite, name, std::move(fn)});
  }
  int RunAll() {
    int failed = 0, ran = 0, skipped = 0;
    for (const auto& c : cases_) {
      if (!ShouldRunCase(c.suite, c.name)) {
        ++skipped;
        continue;
      }
      ++ran;
      try {
        c.fn();
        std::cout << "[  OK  ] " << c.suite << "." << c.name << std::endl;
      } catch (const TestFailure& e) {
        ++failed;
        std::cerr << "[ FAIL ] " << e.suite << "." << e.name << " -> "
                  << e.message << std::endl;
      }
    }
    std::cout << "Ran " << ran << " tests";
    if (skipped > 0) std::cout << " (" << skipped << " skipped)";
    std::cout << ", " << failed << " failed." << std::endl;
    return failed;
  }

 private:
  struct Case {
    std::string suite;
    std::string name;
    std::function<void()> fn;
  };
  std::vector<Case> cases_;
};

inline void Fail(const std::string& message) {
  throw TestFailure{TestScope::Suite(), TestScope::Name(), message};
}

// 环境变量辅助（移植自第三方 test_harness.h）
inline int EnvInt(const char* name, int default_value) {
  const char* v = std::getenv(name);
  if (v == nullptr || *v == '\0') return default_value;
  return std::atoi(v);
}

// GAME_SOAK_SECONDS: LongSoak 时长（秒）。默认 0=不跑长 soak。
inline int SoakSeconds() {
  const int raw = EnvInt("GAME_SOAK_SECONDS", 0);
  if (raw <= 0) return 0;
  if (EnvInt("GAME_SOAK_UNLIMITED", 0) != 0) return raw;
  return raw > 600 ? 600 : raw;
}

inline bool LongSoakEnabled() { return SoakSeconds() > 0; }

inline bool MassAoiStressEnabled() {
  return EnvInt("GAME_AOI_STRESS", 0) != 0 ||
         std::getenv("GAME_AOI_PLAYER_COUNT") != nullptr;
}

inline int MassAoiPlayerCount() {
  const int count = EnvInt("GAME_AOI_PLAYER_COUNT", 128);
  if (!MassAoiStressEnabled() && count > 256) return 256;
  return count > 0 ? count : 128;
}

inline int MassAoiChurnSteps() {
  return EnvInt("GAME_AOI_CHURN_STEPS", 32);
}

inline int ScaleEntityCount() {
  return EnvInt("GAME_SCALE_ENTITY_COUNT", 800);
}

}  // namespace test

#define GAME_TEST_SUITE(suite_name)                                          \
  struct suite_name##_Registrar {                                            \
    static void Register(const char* n, std::function<void()> fn) {          \
      ::test::Runner::Instance().Register(#suite_name, n, std::move(fn));    \
    }                                                                        \
  };

#define GAME_TEST(suite_name, test_name)                                      \
  static void suite_name##_##test_name##_Impl();                             \
  static void suite_name##_##test_name##_Body() {                            \
    ::test::TestScope scope(#suite_name, #test_name);                        \
    suite_name##_##test_name##_Impl();                                       \
  }                                                                          \
  namespace {                                                                \
  struct suite_name##_##test_name##_AutoRegister {                           \
    suite_name##_##test_name##_AutoRegister() {                              \
      suite_name##_Registrar::Register(#test_name,                           \
                                       suite_name##_##test_name##_Body);     \
    }                                                                        \
  } suite_name##_##test_name##_reg;                                          \
  }                                                                          \
  static void suite_name##_##test_name##_Impl()

#define EXPECT_TRUE(cond)                                                    \
  do {                                                                       \
    if (!(cond))                                                             \
      ::test::Fail(#cond " is false @" __FILE__ ":" QTOSTRING(__LINE__));    \
  } while (0)
#define EXPECT_FALSE(cond) EXPECT_TRUE(!(cond))
#define EXPECT_EQ(a, b)                                                      \
  do {                                                                       \
    auto _a = (a);                                                           \
    auto _b = (b);                                                           \
    if (!(_a == _b)) {                                                       \
      std::ostringstream _os;                                                \
      _os << #a " != " #b " (" << _a << " vs " << _b << ")";                 \
      ::test::Fail(_os.str());                                               \
    }                                                                        \
  } while (0)
#define EXPECT_NE(a, b) EXPECT_TRUE((a) != (b))
#define EXPECT_GT(a, b) EXPECT_TRUE((a) > (b))
#define EXPECT_GE(a, b) EXPECT_TRUE((a) >= (b))
#define EXPECT_LT(a, b) EXPECT_TRUE((a) < (b))
#define EXPECT_LE(a, b) EXPECT_TRUE((a) <= (b))
#define EXPECT_NEAR(a, b, eps)                                               \
  do {                                                                       \
    double _da = static_cast<double>(a);                                     \
    double _db = static_cast<double>(b);                                     \
    double _de = static_cast<double>(eps);                                   \
    if (_da < _db - _de || _da > _db + _de) {                                \
      std::ostringstream _os;                                                \
      _os << #a " not near " #b " (" << _da << " vs " << _db                 \
          << " eps=" << _de << ")";                                          \
      ::test::Fail(_os.str());                                               \
    }                                                                        \
  } while (0)
#define QTOSTRING(x) #x
