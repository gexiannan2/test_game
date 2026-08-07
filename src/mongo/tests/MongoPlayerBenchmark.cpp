#include "AsyncMongoDispatcher.h"
#include "MongoClient.h"
#include "MongoConfig.h"

#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <mongocxx/collection.hpp>
#include <mongocxx/pool.hpp>
#include <mongocxx/uri.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace {

using bsoncxx::builder::basic::document;
using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_array;
using bsoncxx::builder::basic::make_document;

constexpr const char* kCollection = "player_benchmark";

enum class BenchmarkMode {
    kRead,
    kWrite,
};

enum class DispatchMode {
    kSync,
    kAsync,
};

struct Options {
    std::vector<std::uint64_t> qps{1000, 10000, 100000};
    std::chrono::seconds duration{10};
    std::chrono::seconds warmup{0};
    std::size_t workers = std::max<std::size_t>(
        2, std::min<std::size_t>(16, std::thread::hardware_concurrency()));
    std::int64_t playerCount = 100000;
    bool runRead = true;
    bool runWrite = true;
    DispatchMode dispatchMode = DispatchMode::kSync;
    std::size_t mongoWorkers = 12;
    std::size_t maxQueuedTasksPerWorker = 4096;
    std::chrono::seconds drainTimeout{30};
    std::string reportPath;
};

struct Metrics {
    std::uint64_t targetOperations = 0;
    std::uint64_t completedOperations = 0;
    std::uint64_t failedOperations = 0;
    std::uint64_t acceptedOperations = 0;
    std::uint64_t rejectedOperations = 0;
    std::uint64_t notStartedOperations = 0;
    std::uint64_t latencySamples = 0;
    double submissionSeconds = 0.0;
    double drainSeconds = 0.0;
    double elapsedSeconds = 0.0;
    std::vector<std::uint64_t> latenciesUs;
    std::string firstError;
};

std::string ModeName(BenchmarkMode mode)
{
    return mode == BenchmarkMode::kRead ? "read" : "write";
}

std::string DispatchModeName(DispatchMode mode)
{
    return mode == DispatchMode::kAsync ? "async" : "sync";
}

std::int64_t ParsePositiveInt64(const std::string& value, const char* option)
{
    try
    {
        const auto parsed = std::stoll(value);
        if (parsed <= 0)
        {
            throw std::invalid_argument("not positive");
        }
        return parsed;
    }
    catch (const std::exception&)
    {
        throw std::invalid_argument(std::string(option) + " 必须是正整数");
    }
}

std::uint64_t CeilDivide(std::uint64_t value, std::uint64_t divisor)
{
    return value / divisor + (value % divisor == 0 ? 0 : 1);
}

std::uint64_t TargetOperations(std::uint64_t targetQps, std::chrono::seconds duration)
{
    const auto seconds = static_cast<std::uint64_t>(duration.count());
    if (targetQps > std::numeric_limits<std::uint64_t>::max() / seconds)
    {
        throw std::invalid_argument("QPS 与时长的乘积超出允许范围");
    }

    const auto operations = targetQps * seconds;
    constexpr std::uint64_t kNanosecondsPerSecond = 1000000000ULL;
    if (operations > std::numeric_limits<std::uint64_t>::max() / kNanosecondsPerSecond)
    {
        throw std::invalid_argument("压测请求数过大，无法安全计算投递时间");
    }
    return operations;
}

void ValidateDuration(std::chrono::seconds duration, const char* option)
{
    constexpr std::int64_t kNanosecondsPerSecond = 1000000000LL;
    if (duration.count() > std::numeric_limits<std::int64_t>::max() / kNanosecondsPerSecond)
    {
        throw std::invalid_argument(std::string(option) + " 过大，无法安全表示为纳秒");
    }
}

void ValidateOptions(const Options& options)
{
    ValidateDuration(options.duration, "--duration");
    if (options.warmup.count() > 0)
    {
        ValidateDuration(options.warmup, "--warmup");
    }

    for (const auto qps : options.qps)
    {
        static_cast<void>(TargetOperations(qps, options.duration));
        if (options.warmup.count() > 0)
        {
            static_cast<void>(TargetOperations(qps, options.warmup));
        }
    }
}

Options ParseOptions(int argc, char* argv[])
{
    Options options;
    bool qpsSpecified = false;
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        const auto requireValue = [&]() -> std::string {
            if (++index >= argc)
            {
                throw std::invalid_argument(argument + " 缺少参数值");
            }
            return argv[index];
        };

        if (argument == "--qps")
        {
            options.qps = {static_cast<std::uint64_t>(
                ParsePositiveInt64(requireValue(), "--qps"))};
            qpsSpecified = true;
        }
        else if (argument == "--duration")
        {
            options.duration = std::chrono::seconds(ParsePositiveInt64(requireValue(), "--duration"));
        }
        else if (argument == "--warmup")
        {
            options.warmup = std::chrono::seconds(ParsePositiveInt64(requireValue(), "--warmup"));
        }
        else if (argument == "--workers")
        {
            options.workers = static_cast<std::size_t>(
                ParsePositiveInt64(requireValue(), "--workers"));
        }
        else if (argument == "--players")
        {
            options.playerCount = ParsePositiveInt64(requireValue(), "--players");
        }
        else if (argument == "--mode")
        {
            const std::string mode = requireValue();
            if (mode == "read")
            {
                options.runRead = true;
                options.runWrite = false;
            }
            else if (mode == "write")
            {
                options.runRead = false;
                options.runWrite = true;
            }
            else if (mode == "all")
            {
                options.runRead = true;
                options.runWrite = true;
            }
            else
            {
                throw std::invalid_argument("--mode 只能是 read、write 或 all");
            }
        }
        else if (argument == "--dispatch")
        {
            const std::string mode = requireValue();
            if (mode == "sync")
            {
                options.dispatchMode = DispatchMode::kSync;
            }
            else if (mode == "async")
            {
                options.dispatchMode = DispatchMode::kAsync;
            }
            else
            {
                throw std::invalid_argument("--dispatch 只能是 sync 或 async");
            }
        }
        else if (argument == "--mongo-workers")
        {
            options.mongoWorkers = static_cast<std::size_t>(
                ParsePositiveInt64(requireValue(), "--mongo-workers"));
        }
        else if (argument == "--queue-per-worker")
        {
            options.maxQueuedTasksPerWorker = static_cast<std::size_t>(
                ParsePositiveInt64(requireValue(), "--queue-per-worker"));
        }
        else if (argument == "--drain-timeout")
        {
            options.drainTimeout = std::chrono::seconds(
                ParsePositiveInt64(requireValue(), "--drain-timeout"));
        }
        else if (argument == "--report")
        {
            options.reportPath = requireValue();
        }
        else if (argument == "--help" || argument == "-h")
        {
            std::cout << "用法：mongo_player_benchmark [--mode read|write|all] "
                         "[--qps N] [--duration 秒] [--workers N] [--players N] "
                         "[--warmup 秒] [--dispatch sync|async] "
                         "[--mongo-workers N] [--queue-per-worker N] "
                         "[--drain-timeout 秒] [--report 结果.csv]\n"
                         "未指定 --qps 时依次执行 1000、10000、100000 QPS。\n";
            std::exit(0);
        }
        else
        {
            throw std::invalid_argument("未知参数：" + argument);
        }
    }

    if (qpsSpecified && options.qps.empty())
    {
        throw std::invalid_argument("--qps 无效");
    }
    ValidateOptions(options);
    return options;
}

bsoncxx::document::value MakePlayer(std::int64_t playerId)
{
    document player;
    player.append(
        kvp("_id", playerId),
        kvp("name", "player_" + std::to_string(playerId)),
        kvp("level", 50),
        kvp("position", make_document(
            kvp("map_id", 101),
            kvp("x", 125.5),
            kvp("y", 6.8),
            kvp("z", 78.2))),
        kvp("attributes", make_document(
            kvp("hp", 5000),
            kvp("mp", 2000),
            kvp("attack", 350))),
        kvp("skills", make_array(make_document(
            kvp("skill_id", 1001),
            kvp("level", 5)))),
        kvp("settings", make_document(kvp("auto_pick", true))),
        kvp("benchmark_version", 1),
        kvp("last_sequence", 0));
    return player.extract();
}

void SeedPlayers(mongocxx::pool& pool, const mongo::MongoConfig& config,
                 const Options& options)
{
    auto client = pool.acquire();
    auto collection = (*client)[config.database][kCollection];
    const auto existing = collection.count_documents(make_document().view());
    if (existing == options.playerCount)
    {
        std::cout << "样本玩家已就绪：" << existing << " 条\n";
        return;
    }

    // 仅重建专用于基准测试的集合，避免影响其他集合。
    collection.delete_many(make_document().view());
    std::cout << "正在准备 " << options.playerCount << " 条玩家样本数据...\n";
    const auto start = std::chrono::steady_clock::now();
    const std::size_t seedWorkers = std::min<std::size_t>(
        options.workers, static_cast<std::size_t>(options.playerCount));
    std::atomic<std::int64_t> nextPlayerId{1};
    std::atomic<std::int64_t> inserted{0};
    std::mutex errorMutex;
    std::string firstError;
    std::vector<std::thread> workers;
    workers.reserve(seedWorkers);

    for (std::size_t worker = 0; worker < seedWorkers; ++worker)
    {
        workers.emplace_back([&]() {
            auto workerClient = pool.acquire();
            auto workerCollection = (*workerClient)[config.database][kCollection];
            while (true)
            {
                const std::int64_t playerId = nextPlayerId.fetch_add(1, std::memory_order_relaxed);
                if (playerId > options.playerCount)
                {
                    return;
                }
                try
                {
                    workerCollection.insert_one(MakePlayer(playerId).view());
                    const std::int64_t completed = inserted.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (completed % 10000 == 0 || completed == options.playerCount)
                    {
                        std::lock_guard<std::mutex> lock(errorMutex);
                        std::cout << "样本准备进度：" << completed << "/" << options.playerCount << '\n';
                    }
                }
                catch (const std::exception& error)
                {
                    std::lock_guard<std::mutex> lock(errorMutex);
                    if (firstError.empty())
                    {
                        firstError = error.what();
                    }
                    return;
                }
            }
        });
    }

    for (auto& worker : workers)
    {
        worker.join();
    }
    if (!firstError.empty())
    {
        throw std::runtime_error("准备玩家样本失败：" + firstError);
    }
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    std::cout << "样本准备完成：" << options.playerCount << " 条，耗时 "
              << std::fixed << std::setprecision(2) << elapsed << " 秒\n";
}

std::uint64_t PercentileUs(std::vector<std::uint64_t> values, double percentile)
{
    if (values.empty())
    {
        return 0;
    }
    std::sort(values.begin(), values.end());
    const auto position = static_cast<std::size_t>(
        std::ceil(percentile * static_cast<double>(values.size()))) - 1;
    return values[std::min(position, values.size() - 1)];
}

void ExecutePlayerOperation(mongo::MongoClient& client, BenchmarkMode mode,
                            std::int64_t playerId, std::uint64_t sequence)
{
    document filter;
    filter.append(kvp("_id", playerId));
    if (mode == BenchmarkMode::kRead)
    {
        const auto result = client.FindOne(kCollection, filter.view());
        if (!result)
        {
            throw std::runtime_error("未找到预置玩家数据");
        }
        return;
    }

    document setFields;
    setFields.append(
        kvp("level", static_cast<std::int32_t>(1 + (sequence % 100))),
        kvp("position", make_document(
            kvp("map_id", 101),
            kvp("x", 125.5 + static_cast<double>(sequence % 100) / 10.0),
            kvp("y", 6.8),
            kvp("z", 78.2))),
        kvp("attributes", make_document(
            kvp("hp", 5000 + static_cast<std::int32_t>(sequence % 500)),
            kvp("mp", 2000),
            kvp("attack", 350))),
        kvp("last_sequence", static_cast<std::int64_t>(sequence)));
    document update;
    update.append(kvp("$set", setFields.extract()));
    const auto result = client.UpdateOne(kCollection, filter.view(), update.view());
    if (result.matchedCount != 1)
    {
        throw std::runtime_error("写入未匹配预置玩家数据");
    }
}

Metrics RunScenario(mongo::MongoClient& client,
                    const Options& options, BenchmarkMode mode, std::uint64_t targetQps)
{
    Metrics metrics;
    metrics.targetOperations = TargetOperations(targetQps, options.duration);
    constexpr std::uint64_t kMaxLatencySamples = 1000000;
    const std::uint64_t latencySamplePeriod = std::max<std::uint64_t>(
        1, CeilDivide(metrics.targetOperations, kMaxLatencySamples));
    const std::uint64_t expectedLatencySamples =
        CeilDivide(metrics.targetOperations, latencySamplePeriod);
    std::atomic<std::uint64_t> nextOperation{0};
    std::atomic<std::uint64_t> completed{0};
    std::atomic<std::uint64_t> failed{0};
    std::mutex resultMutex;
    std::vector<std::thread> threads;
    std::vector<std::vector<std::uint64_t>> workerLatencies(options.workers);
    const auto launchAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    const auto finishAt = launchAt + options.duration;

    for (std::size_t worker = 0; worker < options.workers; ++worker)
    {
        threads.emplace_back([&, worker]() {
            auto& latencies = workerLatencies[worker];
            latencies.reserve(static_cast<std::size_t>(
                expectedLatencySamples / options.workers + 1));

            while (true)
            {
                const std::uint64_t sequence = nextOperation.fetch_add(1, std::memory_order_relaxed);
                if (sequence >= metrics.targetOperations)
                {
                    return;
                }

                const auto dueAt = launchAt + std::chrono::nanoseconds(
                    (sequence * 1000000000ULL) / targetQps);
                if (dueAt >= finishAt)
                {
                    return;
                }
                std::this_thread::sleep_until(dueAt);
                if (std::chrono::steady_clock::now() >= finishAt)
                {
                    return;
                }
                const auto begin = std::chrono::steady_clock::now();
                const std::int64_t playerId =
                    static_cast<std::int64_t>(sequence % static_cast<std::uint64_t>(options.playerCount)) + 1;

                try
                {
                    ExecutePlayerOperation(client, mode, playerId, sequence);
                    completed.fetch_add(1, std::memory_order_relaxed);
                }
                catch (const std::exception& error)
                {
                    failed.fetch_add(1, std::memory_order_relaxed);
                    std::lock_guard<std::mutex> lock(resultMutex);
                    if (metrics.firstError.empty())
                    {
                        metrics.firstError = error.what();
                    }
                }

                const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - begin).count();
                if (sequence % latencySamplePeriod == 0)
                {
                    latencies.push_back(
                        static_cast<std::uint64_t>(std::max<std::int64_t>(0, elapsedUs)));
                }
            }
        });
    }

    for (auto& thread : threads)
    {
        thread.join();
    }

    metrics.elapsedSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - launchAt).count();
    metrics.completedOperations = completed.load(std::memory_order_relaxed);
    metrics.failedOperations = failed.load(std::memory_order_relaxed);
    metrics.acceptedOperations = metrics.completedOperations + metrics.failedOperations;
    metrics.submissionSeconds = metrics.elapsedSeconds;
    metrics.notStartedOperations = metrics.targetOperations -
        std::min(metrics.targetOperations,
                 metrics.completedOperations + metrics.failedOperations);
    for (auto& latencies : workerLatencies)
    {
        metrics.latenciesUs.insert(
            metrics.latenciesUs.end(),
            std::make_move_iterator(latencies.begin()),
            std::make_move_iterator(latencies.end()));
    }
    metrics.latencySamples = metrics.latenciesUs.size();
    return metrics;
}

Metrics RunAsyncScenario(const mongo::MongoConfig& config,
                         const Options& options, BenchmarkMode mode,
                         std::uint64_t targetQps)
{
    Metrics metrics;
    metrics.targetOperations = TargetOperations(targetQps, options.duration);
    constexpr std::uint64_t kMaxLatencySamples = 100000;
    const std::uint64_t latencySamplePeriod = std::max<std::uint64_t>(
        1, CeilDivide(metrics.targetOperations, kMaxLatencySamples));
    const std::uint64_t expectedLatencySamples =
        CeilDivide(metrics.targetOperations, latencySamplePeriod);
    std::atomic<std::uint64_t> nextOperation{0};
    std::atomic<std::uint64_t> accepted{0};
    std::atomic<std::uint64_t> rejected{0};
    std::mutex latencyMutex;
    std::vector<std::uint64_t> latenciesUs;
    latenciesUs.reserve(static_cast<std::size_t>(expectedLatencySamples));
    std::mutex errorMutex;
    std::string firstError;

    mongo::AsyncMongoDispatcher dispatcher(
        config,
        {.workerCount = options.mongoWorkers,
         .maxQueuedTasksPerWorker = options.maxQueuedTasksPerWorker},
        [&](std::int64_t, std::exception_ptr error) {
            std::lock_guard<std::mutex> lock(errorMutex);
            if (!firstError.empty())
            {
                return;
            }
            try
            {
                std::rethrow_exception(error);
            }
            catch (const std::exception& exception)
            {
                firstError = exception.what();
            }
            catch (...)
            {
                firstError = "未知异步任务异常";
            }
        });

    const auto metricsBefore = dispatcher.Metrics();
    std::vector<std::thread> threads;
    threads.reserve(options.workers);
    const auto launchAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    const auto finishAt = launchAt + options.duration;

    for (std::size_t worker = 0; worker < options.workers; ++worker)
    {
        threads.emplace_back([&, worker]() {
            while (true)
            {
                const std::uint64_t sequence = nextOperation.fetch_add(1, std::memory_order_relaxed);
                if (sequence >= metrics.targetOperations)
                {
                    return;
                }

                const auto dueAt = launchAt + std::chrono::nanoseconds(
                    (sequence * 1000000000ULL) / targetQps);
                if (dueAt >= finishAt)
                {
                    return;
                }
                std::this_thread::sleep_until(dueAt);
                if (std::chrono::steady_clock::now() >= finishAt)
                {
                    return;
                }

                const std::int64_t playerId = static_cast<std::int64_t>(
                    sequence % static_cast<std::uint64_t>(options.playerCount)) + 1;
                const bool sampleLatency = sequence % latencySamplePeriod == 0;
                const auto submittedAt = std::chrono::steady_clock::now();
                const bool wasAccepted = dispatcher.Post(
                    playerId,
                    [&, mode, playerId, sequence, sampleLatency, submittedAt](
                        mongo::MongoClient& client) {
                        try
                        {
                            ExecutePlayerOperation(client, mode, playerId, sequence);
                        }
                        catch (...)
                        {
                            if (sampleLatency)
                            {
                                const auto elapsedUs = std::chrono::duration_cast<
                                    std::chrono::microseconds>(
                                    std::chrono::steady_clock::now() - submittedAt).count();
                                std::lock_guard<std::mutex> lock(latencyMutex);
                                latenciesUs.push_back(static_cast<std::uint64_t>(
                                    std::max<std::int64_t>(0, elapsedUs)));
                            }
                            throw;
                        }
                        if (sampleLatency)
                        {
                            const auto elapsedUs = std::chrono::duration_cast<
                                std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - submittedAt).count();
                            std::lock_guard<std::mutex> lock(latencyMutex);
                            latenciesUs.push_back(static_cast<std::uint64_t>(
                                std::max<std::int64_t>(0, elapsedUs)));
                        }
                    });
                if (wasAccepted)
                {
                    accepted.fetch_add(1, std::memory_order_relaxed);
                }
                else
                {
                    rejected.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& thread : threads)
    {
        thread.join();
    }

    const auto submissionFinishedAt = std::chrono::steady_clock::now();
    if (!dispatcher.WaitForIdle(options.drainTimeout))
    {
        throw std::runtime_error("异步投递队列没有在排空超时前完成");
    }
    const auto completedAt = std::chrono::steady_clock::now();
    const auto metricsAfter = dispatcher.Metrics();
    dispatcher.Stop();

    metrics.acceptedOperations = accepted.load(std::memory_order_relaxed);
    metrics.rejectedOperations = rejected.load(std::memory_order_relaxed);
    metrics.completedOperations = metricsAfter.completed - metricsBefore.completed;
    metrics.failedOperations = metricsAfter.failed - metricsBefore.failed;
    if (metrics.acceptedOperations != metrics.completedOperations + metrics.failedOperations)
    {
        throw std::runtime_error("异步投递任务计数不一致");
    }
    metrics.notStartedOperations = metrics.targetOperations - std::min(
        metrics.targetOperations, metrics.acceptedOperations + metrics.rejectedOperations);
    metrics.submissionSeconds = std::chrono::duration<double>(
        submissionFinishedAt - launchAt).count();
    metrics.drainSeconds = std::chrono::duration<double>(
        completedAt - submissionFinishedAt).count();
    metrics.elapsedSeconds = std::chrono::duration<double>(completedAt - launchAt).count();
    metrics.latenciesUs = std::move(latenciesUs);
    metrics.latencySamples = metrics.latenciesUs.size();
    metrics.firstError = std::move(firstError);
    return metrics;
}

void WriteMetricsCsv(const std::string& path, DispatchMode dispatchMode,
                     BenchmarkMode mode, std::uint64_t targetQps, const Metrics& metrics)
{
    const bool writeHeader = !std::filesystem::exists(path) ||
        std::filesystem::file_size(path) == 0;
    std::ofstream output(path, std::ios::app);
    if (!output)
    {
        throw std::runtime_error("无法写入基准测试报告：" + path);
    }
    if (writeHeader)
    {
        output << "mode,dispatch,target_qps,accepted,rejected,completed,failed,not_started,"
                  "submission_seconds,drain_seconds,elapsed_seconds,actual_qps,latency_samples,"
                  "p50_us,p95_us,p99_us,max_us\n";
    }
    const auto total = metrics.completedOperations + metrics.failedOperations;
    const double actualQps = metrics.elapsedSeconds > 0.0
        ? static_cast<double>(total) / metrics.elapsedSeconds
        : 0.0;
    output << ModeName(mode) << ',' << DispatchModeName(dispatchMode) << ',' << targetQps
           << ',' << metrics.acceptedOperations << ',' << metrics.rejectedOperations
           << ',' << metrics.completedOperations << ',' << metrics.failedOperations
           << ',' << metrics.notStartedOperations << ',' << std::fixed << std::setprecision(3)
           << metrics.submissionSeconds << ',' << metrics.drainSeconds << ','
           << metrics.elapsedSeconds << ',' << actualQps << ','
           << metrics.latencySamples << ','
           << PercentileUs(metrics.latenciesUs, 0.50) << ','
           << PercentileUs(metrics.latenciesUs, 0.95) << ','
           << PercentileUs(metrics.latenciesUs, 0.99) << ','
           << PercentileUs(metrics.latenciesUs, 1.0) << '\n';
}

template <typename ScenarioRunner>
void RunWarmupIfNeeded(const Options& options, BenchmarkMode mode,
                       std::uint64_t targetQps, ScenarioRunner&& runScenario)
{
    if (options.warmup.count() == 0)
    {
        return;
    }
    Options warmupOptions = options;
    warmupOptions.duration = options.warmup;
    std::cout << "\n[" << ModeName(mode) << "] 预热=" << options.warmup.count()
              << " 秒，目标=" << targetQps << " QPS\n";
    const auto metrics = runScenario(warmupOptions, mode, targetQps);
    if (metrics.failedOperations != 0 || metrics.rejectedOperations != 0)
    {
        throw std::runtime_error("预热期间出现失败或队列拒绝：" + metrics.firstError);
    }
}

void PrintMetrics(DispatchMode dispatchMode, BenchmarkMode mode,
                  std::uint64_t targetQps, const Metrics& metrics)
{
    const auto total = metrics.completedOperations + metrics.failedOperations;
    const double actualQps = metrics.elapsedSeconds > 0.0
        ? static_cast<double>(total) / metrics.elapsedSeconds
        : 0.0;
    const double successRate = total > 0
        ? 100.0 * static_cast<double>(metrics.completedOperations) / static_cast<double>(total)
        : 0.0;
    const double submissionQps = metrics.submissionSeconds > 0.0
        ? static_cast<double>(metrics.acceptedOperations) / metrics.submissionSeconds
        : 0.0;

    std::cout << "\n[" << ModeName(mode) << "] 模式=" << DispatchModeName(dispatchMode)
              << "，目标=" << targetQps << " QPS"
              << "，请求=" << metrics.targetOperations
              << "，已投递=" << metrics.acceptedOperations
              << "，队列拒绝=" << metrics.rejectedOperations
              << "，未启动=" << metrics.notStartedOperations << '\n'
              << "投递=" << std::fixed << std::setprecision(1) << submissionQps << " QPS"
              << "，落库=" << actualQps << " QPS"
              << "，成功=" << metrics.completedOperations
              << "，失败=" << metrics.failedOperations
              << "，成功率=" << std::setprecision(2) << successRate << "%"
              << "，排空=" << std::setprecision(3) << metrics.drainSeconds << " 秒\n"
              << "延迟(us)：P50=" << PercentileUs(metrics.latenciesUs, 0.50)
              << "，P95=" << PercentileUs(metrics.latenciesUs, 0.95)
              << "，P99=" << PercentileUs(metrics.latenciesUs, 0.99)
              << "，最大=" << PercentileUs(metrics.latenciesUs, 1.0)
              << "，样本=" << metrics.latencySamples << '\n';
    if (!metrics.firstError.empty())
    {
        std::cout << "首个错误：" << metrics.firstError << '\n';
    }
}

} // namespace

int main(int argc, char* argv[])
{
    try
    {
#ifdef _WIN32
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
#endif
        const Options options = ParseOptions(argc, argv);
        const auto config = mongo::MongoConfig::FromEnvironment();
        // MongoClient 负责创建进程唯一的 MongoDB Driver 实例。
        mongo::MongoClient client(config);
        mongocxx::pool seedPool{mongocxx::uri{config.EffectiveUri()}};

        auto seedClient = seedPool.acquire();
        (*seedClient)[config.database].run_command(make_document(kvp("ping", 1)).view());
        seedClient = nullptr;

        std::cout << "MongoDB 玩家读写基准测试\n"
                  << "数据库=" << config.database << "，集合=" << kCollection
                  << "，并发=" << options.workers << "，时长=" << options.duration.count()
                  << " 秒，玩家数=" << options.playerCount
                  << "，连接池=" << config.minPoolSize << '-' << config.maxPoolSize
                  << "，请求上限=" << config.maxInFlightRequests
                  << "，投递模式=" << DispatchModeName(options.dispatchMode)
                  << "，MongoDB 工作线程=" << options.mongoWorkers
                  << "，单线程队列=" << options.maxQueuedTasksPerWorker << "\n";
        SeedPlayers(seedPool, config, options);
        client.Ping();

        const auto runScenario = [&](const Options& runOptions, BenchmarkMode runMode,
                                     std::uint64_t targetQps) {
            if (options.dispatchMode == DispatchMode::kAsync)
            {
                return RunAsyncScenario(config, runOptions, runMode, targetQps);
            }
            return RunScenario(client, runOptions, runMode, targetQps);
        };

        for (const auto targetQps : options.qps)
        {
            if (options.runWrite)
            {
                RunWarmupIfNeeded(options, BenchmarkMode::kWrite, targetQps, runScenario);
                const auto metrics = runScenario(options, BenchmarkMode::kWrite, targetQps);
                PrintMetrics(options.dispatchMode, BenchmarkMode::kWrite, targetQps, metrics);
                if (!options.reportPath.empty())
                {
                    WriteMetricsCsv(
                        options.reportPath, options.dispatchMode,
                        BenchmarkMode::kWrite, targetQps, metrics);
                }
            }
            if (options.runRead)
            {
                RunWarmupIfNeeded(options, BenchmarkMode::kRead, targetQps, runScenario);
                const auto metrics = runScenario(options, BenchmarkMode::kRead, targetQps);
                PrintMetrics(options.dispatchMode, BenchmarkMode::kRead, targetQps, metrics);
                if (!options.reportPath.empty())
                {
                    WriteMetricsCsv(
                        options.reportPath, options.dispatchMode,
                        BenchmarkMode::kRead, targetQps, metrics);
                }
            }
        }
        if (options.dispatchMode == DispatchMode::kSync)
        {
            const auto clientMetrics = client.Metrics();
            std::cout << "\n客户端指标：提交=" << clientMetrics.submitted
                      << "，完成=" << clientMetrics.completed
                      << "，失败=" << clientMetrics.failed
                      << "，背压拒绝=" << clientMetrics.rejected
                      << "，活动请求=" << clientMetrics.active << '\n';
        }
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "MongoDB 玩家基准测试失败：" << error.what() << '\n';
        return 1;
    }
}
