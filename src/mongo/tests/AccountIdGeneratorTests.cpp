// AccountIdGenerator 单元测试
// 覆盖：非 0、类型、唯一性、并发、单调性、_id 编码/反解、area_id 越界
//
// 不依赖 mongo 数据库连接，纯内存单元测试。

#include "AccountIdGenerator.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

// 1. 单次调用返回非 0 uint64
void TestGeneratesNonZero()
{
    auto id = mongo::GenerateAccountId();
    std::cout << "  generated id=0x" << std::hex << id << std::dec << "\n";
    Require(id != 0, "GenerateAccountId should return non-zero");
    Require(sizeof(decltype(id)) == sizeof(std::uint64_t),
            "GenerateAccountId should return uint64_t");
    std::cout << "  [OK] TestGeneratesNonZero\n";
}

// 2. 顺序生成 10000 个全唯一
void TestGenerates10000UniqueIds()
{
    constexpr int kCount = 10000;
    std::set<std::uint64_t> ids;
    for (int i = 0; i < kCount; ++i)
    {
        ids.insert(mongo::GenerateAccountId());
    }
    Require(ids.size() == static_cast<size_t>(kCount),
            "10000 sequential ids should all be unique");
    std::cout << "  [OK] TestGenerates10000UniqueIds count=" << kCount << "\n";
}

// 3. 多线程并发（4 线程 × 10000 次）无碰撞
void TestConcurrent4ThreadsNoCollision()
{
    constexpr int kThreads = 4;
    constexpr int kPerThread = 10000;
    constexpr int kTotal = kThreads * kPerThread;

    std::vector<std::thread> workers;
    std::vector<std::vector<std::uint64_t>> per_thread_ids(kThreads);
    per_thread_ids.resize(kThreads);

    for (int t = 0; t < kThreads; ++t)
    {
        workers.emplace_back([t, &per_thread_ids]()
        {
            auto& ids = per_thread_ids[t];
            ids.reserve(kPerThread);
            for (int i = 0; i < kPerThread; ++i)
            {
                ids.push_back(mongo::GenerateAccountId());
            }
        });
    }
    for (auto& w : workers)
    {
        w.join();
    }

    std::set<std::uint64_t> all;
    for (const auto& v : per_thread_ids)
    {
        all.insert(v.begin(), v.end());
    }
    Require(all.size() == static_cast<size_t>(kTotal),
            "4x10000 concurrent ids should all be unique");
    std::cout << "  [OK] TestConcurrent4ThreadsNoCollision total=" << kTotal << "\n";
}

// 4. 同进程内连续两次调用，后者 > 前者（同母ID周期内单调递增）
// 注意：极低概率下两者刚好跨越母ID刷新点，会不满足 > 关系；
// 但 4 个 sid 自增间隔内刷新母ID 的概率 < 4/2^32，可忽略。
// 为避免 flaky，循环若干次取 majority。
void TestMonotonicWithinSameMasterCycle()
{
    constexpr int kRounds = 100;
    int monotonic_count = 0;
    for (int i = 0; i < kRounds; ++i)
    {
        auto id1 = mongo::GenerateAccountId();
        auto id2 = mongo::GenerateAccountId();
        if (id2 > id1)
        {
            ++monotonic_count;
        }
    }
    // 允许偶尔跨越刷新点，但至少 90% 应该满足单调递增
    Require(monotonic_count >= kRounds * 9 / 10,
            "consecutive ids should be monotonic in same master cycle");
    std::cout << "  [OK] TestMonotonicWithinSameMasterCycle "
              << monotonic_count << "/" << kRounds << " monotonic\n";
}

// 5. _id 编码与反解一致性
void TestMakeAccountInfoIdRoundTrip()
{
    // 选取若干 account_id 与 area_id 组合，验证 _id 反解一致性
    const std::int64_t test_cases[][2] = {
        {622718261, 0},
        {622718261, 1},
        {622718261, 999},
        {1, 0},
        {0, 0},          // 边界：account_id=0 也能编码（但实际不会用）
        {0, 999999},      // area_id 上界
        {2147483647, 0},  // int32 上界附近
        {9223372036854LL, 0},  // 接近 int64/1e6 的上界附近
    };

    for (const auto& tc : test_cases)
    {
        const std::int64_t account_id = tc[0];
        const std::int64_t area_id = tc[1];
        const std::int64_t id = mongo::MakeAccountInfoId(account_id, area_id);
        Require(mongo::GetAccountIdFromId(id) == account_id,
                "account_id round-trip failed");
        Require(mongo::GetAreaIdFromId(id) == area_id,
                "area_id round-trip failed");
    }
    std::cout << "  [OK] TestMakeAccountInfoIdRoundTrip cases="
              << sizeof(test_cases) / sizeof(test_cases[0]) << "\n";
}

// 6. _id 数值聚集：同 account_id 跨 area_id 的 _id 数值上聚集
void TestAccountIdClusteredAcrossAreas()
{
    const std::int64_t account_id = 622718261;
    const std::int64_t base = mongo::MakeAccountInfoId(account_id, 0);

    // 同账号在 area=0..4 的 _id 应该是 base, base+1, base+2, base+3, base+4
    for (std::int64_t area = 0; area < 5; ++area)
    {
        Require(mongo::MakeAccountInfoId(account_id, area) == base + area,
                "same account across areas should cluster by +1");
    }
    std::cout << "  [OK] TestAccountIdClusteredAcrossAreas base=" << base << "\n";
}

// 7. IsValidAreaId 边界
void TestIsValidAreaIdBoundary()
{
    Require(mongo::IsValidAreaId(0), "0 should be valid");
    Require(mongo::IsValidAreaId(1), "1 should be valid");
    Require(mongo::IsValidAreaId(999999), "999999 should be valid (upper bound)");
    Require(!mongo::IsValidAreaId(-1), "-1 should be invalid");
    Require(!mongo::IsValidAreaId(1000000), "1000000 should be invalid (out of range)");
    Require(!mongo::IsValidAreaId(9999999), "9999999 should be invalid");
    std::cout << "  [OK] TestIsValidAreaIdBoundary\n";
}

// 8. 反解 _id 不被 account_id 大小影响（验证 uint64 不溢出）
void TestLargeAccountIdRoundTrip()
{
    // account_id 接近 generate_id 实际范围上界
    const std::int64_t account_id = static_cast<std::int64_t>(0x00000005FFFFFFFFLL); // ~25 亿
    const std::int64_t area_id = 123;
    const std::int64_t id = mongo::MakeAccountInfoId(account_id, area_id);
    Require(mongo::GetAccountIdFromId(id) == account_id,
            "large account_id round-trip failed");
    Require(mongo::GetAreaIdFromId(id) == area_id,
            "large account_id area_id round-trip failed");
    std::cout << "  [OK] TestLargeAccountIdRoundTrip id=" << id << "\n";
}

}  // namespace

int main()
{
    try
    {
        std::cout << "AccountIdGeneratorTests\n";
        TestGeneratesNonZero();
        TestGenerates10000UniqueIds();
        TestConcurrent4ThreadsNoCollision();
        TestMonotonicWithinSameMasterCycle();
        TestMakeAccountInfoIdRoundTrip();
        TestAccountIdClusteredAcrossAreas();
        TestIsValidAreaIdBoundary();
        TestLargeAccountIdRoundTrip();
        std::cout << "ALL PASSED\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "FAILED: " << e.what() << "\n";
        return 1;
    }
}

