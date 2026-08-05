#include "AsyncMongoDispatcher.h"
#include "MongoClient.h"
#include "MongoConfig.h"
#include "PlayerMongoStorage.h"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace {

using bsoncxx::builder::basic::document;
using bsoncxx::builder::basic::kvp;

void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

std::string BatchId()
{
    return "mongo_async_test_" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
}

void RunAffinityAndPersistenceTest()
{
    auto config = mongo::MongoConfig::FromEnvironment();
    config.minPoolSize = 1;
    config.maxPoolSize = 12;
    config.maxInFlightRequests = 24;

    constexpr std::size_t kWorkerCount = 12;
    constexpr std::int64_t kFirstPlayerId = 20001;
    constexpr std::int64_t kPlayerCount = 24;
    constexpr std::int32_t kWritesPerPlayer = 6;
    const std::string batch = BatchId();
    std::mutex resultMutex;
    std::unordered_map<std::int64_t, std::thread::id> playerWorkerIds;
    std::unordered_set<std::thread::id> workerThreadIds;
    std::exception_ptr firstTaskError;

    mongo::AsyncMongoDispatcher dispatcher(
        config,
        {.workerCount = kWorkerCount, .maxQueuedTasksPerWorker = 128},
        [&](std::int64_t, std::exception_ptr error) {
            std::lock_guard<std::mutex> lock(resultMutex);
            if (!firstTaskError)
            {
                firstTaskError = error;
            }
        });

    for (std::int64_t playerId = kFirstPlayerId;
         playerId < kFirstPlayerId + kPlayerCount;
         ++playerId)
    {
        for (std::int32_t sequence = 1; sequence <= kWritesPerPlayer; ++sequence)
        {
            const bool accepted = dispatcher.Post(
                playerId,
                [&, batch, playerId, sequence](mongo::MongoClient& client) {
                    {
                        std::lock_guard<std::mutex> lock(resultMutex);
                        const auto [iterator, inserted] = playerWorkerIds.emplace(
                            playerId, std::this_thread::get_id());
                        workerThreadIds.emplace(std::this_thread::get_id());
                        if (!inserted)
                        {
                            Require(iterator->second == std::this_thread::get_id(),
                                    "同一玩家没有固定到同一个 MongoDB 工作线程");
                        }
                    }

                    document filter;
                    filter.append(kvp("_id", playerId));
                    document fields;
                    fields.append(
                        kvp("batch", batch),
                        kvp("last_sequence", sequence),
                        kvp("saved_by_async_dispatcher", true));
                    document update;
                    update.append(kvp("$set", fields.extract()));
                    const auto result = client.UpdateOne(
                        "async_dispatch_cases", filter.view(), update.view(), true);
                    Require(result.matchedCount == 1 || result.upserted,
                            "异步任务没有写入玩家文档");
                });
            Require(accepted, "异步任务被意外拒绝");
        }
    }

    Require(dispatcher.WaitForIdle(std::chrono::seconds(20)), "异步任务没有在超时前排空");
    dispatcher.Stop();

    if (firstTaskError)
    {
        std::rethrow_exception(firstTaskError);
    }
    const auto metrics = dispatcher.Metrics();
    const std::uint64_t expectedTasks =
        static_cast<std::uint64_t>(kPlayerCount * kWritesPerPlayer);
    Require(metrics.posted == expectedTasks, "异步任务投递计数不正确");
    Require(metrics.completed == expectedTasks, "异步任务完成计数不正确");
    Require(metrics.failed == 0 && metrics.rejected == 0,
            "异步任务出现失败或背压拒绝");
    Require(metrics.queued == 0 && metrics.active == 0,
            "异步任务队列没有完全排空");
    Require(playerWorkerIds.size() == static_cast<std::size_t>(kPlayerCount),
            "玩家工作线程亲和性记录不完整");
    Require(workerThreadIds.size() == kWorkerCount,
            "12 个 MongoDB 工作线程没有全部参与任务执行");

    mongo::MongoClient verifier(config);
    for (std::int64_t playerId = kFirstPlayerId;
         playerId < kFirstPlayerId + kPlayerCount;
         ++playerId)
    {
        document filter;
        filter.append(kvp("_id", playerId));
        const auto player = verifier.FindOne("async_dispatch_cases", filter.view());
        Require(player.has_value(), "异步任务写入后的玩家文档不存在");
        Require(player->view()["batch"].get_string().value == batch,
                "异步任务写入了错误的测试批次");
        Require(player->view()["last_sequence"].get_int32().value == kWritesPerPlayer,
                "同一玩家的异步任务没有按 FIFO 顺序执行");
    }

    document batchFilter;
    batchFilter.append(kvp("batch", batch));
    Require(verifier.DeleteMany("async_dispatch_cases", batchFilter.view()).deletedCount ==
                kPlayerCount,
            "异步任务测试数据清理失败");
}

void RunBackpressureTest()
{
    auto config = mongo::MongoConfig::FromEnvironment();
    std::mutex mutex;
    std::condition_variable condition;
    bool firstTaskStarted = false;
    bool releaseFirstTask = false;

    mongo::AsyncMongoDispatcher dispatcher(
        config, {.workerCount = 1, .maxQueuedTasksPerWorker = 1});
    Require(dispatcher.Post(1, [&](mongo::MongoClient&) {
        std::unique_lock<std::mutex> lock(mutex);
        firstTaskStarted = true;
        condition.notify_one();
        condition.wait(lock, [&]() { return releaseFirstTask; });
    }), "第一个背压测试任务投递失败");

    {
        std::unique_lock<std::mutex> lock(mutex);
        Require(condition.wait_for(lock, std::chrono::seconds(2), [&]() {
                    return firstTaskStarted;
                }),
                "背压测试工作线程没有启动");
    }
    Require(dispatcher.Post(1, [](mongo::MongoClient&) {}),
            "队列未满时任务被拒绝");
    Require(!dispatcher.Post(1, [](mongo::MongoClient&) {}),
            "队列已满时任务没有被拒绝");

    {
        std::lock_guard<std::mutex> lock(mutex);
        releaseFirstTask = true;
    }
    condition.notify_one();
    Require(dispatcher.WaitForIdle(std::chrono::seconds(5)), "背压测试任务没有排空");
    dispatcher.Stop();

    const auto metrics = dispatcher.Metrics();
    Require(metrics.posted == 2 && metrics.completed == 2,
            "背压测试完成计数不正确");
    Require(metrics.rejected == 1 && metrics.failed == 0,
            "背压测试拒绝或失败计数不正确");
}

void RunIdleAndStopReentryTest()
{
    auto config = mongo::MongoConfig::FromEnvironment();
    std::mutex mutex;
    std::condition_variable condition;
    bool taskStarted = false;
    bool releaseTask = false;
    std::atomic<bool> stopReturned{false};

    mongo::AsyncMongoDispatcher dispatcher(
        config, {.workerCount = 1, .maxQueuedTasksPerWorker = 4});
    Require(dispatcher.Post(1, [&](mongo::MongoClient&) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            taskStarted = true;
        }
        condition.notify_one();

        {
            std::unique_lock<std::mutex> lock(mutex);
            condition.wait(lock, [&]() { return releaseTask; });
        }

        // 工作线程内调用 Stop 只能请求停止，不能尝试 join 自身。
        dispatcher.Stop();
        stopReturned.store(true, std::memory_order_release);
    }), "停止重入测试任务投递失败");

    {
        std::unique_lock<std::mutex> lock(mutex);
        Require(condition.wait_for(lock, std::chrono::seconds(2), [&]() {
                    return taskStarted;
                }),
                "停止重入测试工作线程没有启动");
    }
    Require(!dispatcher.WaitForIdle(std::chrono::milliseconds(50)),
            "活动任务执行期间 WaitForIdle 不应提前返回");

    {
        std::lock_guard<std::mutex> lock(mutex);
        releaseTask = true;
    }
    condition.notify_one();

    Require(dispatcher.WaitForIdle(std::chrono::seconds(5)),
            "停止重入测试任务没有排空");
    Require(stopReturned.load(std::memory_order_acquire),
            "工作线程内调用 Stop 没有正常返回");
    dispatcher.Stop();

    const auto metrics = dispatcher.Metrics();
    Require(metrics.posted == 1 && metrics.completed == 1 && metrics.failed == 0,
            "停止重入测试任务状态不正确");
}

void RunErrorHandlerStopReentryTest()
{
    auto config = mongo::MongoConfig::FromEnvironment();
    mongo::AsyncMongoDispatcher* dispatcherPointer = nullptr;
    std::atomic<bool> handlerReturned{false};

    mongo::AsyncMongoDispatcher dispatcher(
        config,
        {.workerCount = 1, .maxQueuedTasksPerWorker = 4},
        [&](std::int64_t, std::exception_ptr) {
            dispatcherPointer->Stop();
            handlerReturned.store(true, std::memory_order_release);
        });
    dispatcherPointer = &dispatcher;

    Require(dispatcher.Post(1, [](mongo::MongoClient&) {
        throw std::runtime_error("用于验证错误回调停止重入的预期异常");
    }), "错误回调停止重入测试任务投递失败");
    Require(dispatcher.WaitForIdle(std::chrono::seconds(5)),
            "错误回调停止重入测试任务没有排空");
    Require(handlerReturned.load(std::memory_order_acquire),
            "错误回调中调用 Stop 没有正常返回");
    dispatcher.Stop();

    const auto metrics = dispatcher.Metrics();
    Require(metrics.posted == 1 && metrics.completed == 0 && metrics.failed == 1,
            "错误回调停止重入测试任务状态不正确");
}

void RunPlayerStorageTest()
{
    auto config = mongo::MongoConfig::FromEnvironment();
    const std::int64_t playerId = static_cast<std::int64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    mongo::PlayerMongoStorage storage(
        config,
        {
            .collection = "player_persistence_cases",
            .dispatcher = {.workerCount = 2, .maxQueuedTasksPerWorker = 16},
        });

    ::entity_player_data snapshot;
    snapshot.mutable_base()->set_id(static_cast<uint64_t>(playerId));
    snapshot.mutable_base()->mutable_pos()->set_x(125.5f);
    snapshot.mutable_base()->mutable_pos()->set_y(6.8f);
    snapshot.mutable_base()->mutable_pos()->set_z(78.2f);
    snapshot.mutable_base()->set_cfg_id(101);
    snapshot.mutable_base()->set_type(::ENTITY_PLAYER);
    snapshot.mutable_base_data()->set_lv(50);

    const uint64_t sequence = 7;
    Require(storage.PostSave(std::move(snapshot), sequence), "玩家快照投递失败");
    storage.RequestStop();
    Require(storage.WaitForIdle(std::chrono::seconds(10)), "玩家快照没有在超时前落库");
    storage.Stop();

    mongo::MongoClient verifier(config);
    document filter;
    filter.append(kvp("_id", playerId));
    const auto player = verifier.FindOne("player_persistence_cases", filter.view());
    Require(player.has_value(), "玩家异步快照文档不存在");
    const auto view = player->view();
    Require(view["last_sequence"].get_int64().value == 7,
            "玩家快照序号不正确");
    // 明文 BSON 嵌套字段校验
    const auto baseView = view["base"].get_document().view();
    const auto baseDataView = view["base_data"].get_document().view();
    const auto posView = baseView["pos"].get_document().view();
    Require(static_cast<uint64_t>(baseView["id"].get_int64().value)
                == static_cast<uint64_t>(playerId),
            "玩家快照 id 不正确");
    Require(posView["x"].get_double().value == 125.5, "玩家快照位置 x 不正确");
    Require(static_cast<uint32_t>(baseView["cfg_id"].get_int64().value) == 101,
            "玩家快照地图不正确");
    Require(static_cast<uint32_t>(baseDataView["lv"].get_int64().value) == 50,
            "玩家快照等级不正确");
    Require(verifier.DeleteOne("player_persistence_cases", filter.view()).deletedCount == 1,
            "玩家快照测试数据清理失败");
}

} // namespace

int main()
{
    try
    {
        RunAffinityAndPersistenceTest();
        RunBackpressureTest();
        RunIdleAndStopReentryTest();
        RunErrorHandlerStopReentryTest();
        RunPlayerStorageTest();
        std::cout << "MongoDB async dispatcher integration test passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "MongoDB async dispatcher integration test failed: "
                  << error.what() << '\n';
        return 1;
    }
}
