#pragma once

#include "MongoClient.h"
#include "MongoConfig.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace mongo
{
    struct AsyncMongoDispatcherOptions
    {
        std::size_t workerCount = 12;
        std::size_t maxQueuedTasksPerWorker = 4096;
    };

    struct AsyncMongoDispatcherMetrics
    {
        std::uint64_t posted = 0;
        std::uint64_t completed = 0;
        std::uint64_t failed = 0;
        std::uint64_t rejected = 0;
        std::uint64_t queued = 0;
        std::uint64_t active = 0;
    };

    class AsyncMongoDispatcher final
    {
        public:
            // 任务会在稍后的 MongoDB 工作线程执行；调用方必须捕获值或保证引用对象的生命周期。
            using Task = std::function<void(MongoClient&)>;
            // 错误回调可能由多个 MongoDB 工作线程并发调用。
            using ErrorHandler = std::function<void(std::int64_t playerId, std::exception_ptr error)>;

            AsyncMongoDispatcher(
                MongoConfig config,
                AsyncMongoDispatcherOptions options = {},
                ErrorHandler errorHandler = {});
            ~AsyncMongoDispatcher();

            AsyncMongoDispatcher(const AsyncMongoDispatcher&) = delete;
            AsyncMongoDispatcher& operator=(const AsyncMongoDispatcher&) = delete;
            AsyncMongoDispatcher(AsyncMongoDispatcher&&) = delete;
            AsyncMongoDispatcher& operator=(AsyncMongoDispatcher&&) = delete;

            // 按玩家固定路由；同一玩家的已接受任务在同一工作线程内 FIFO 执行。
            bool Post(std::int64_t playerId, Task task);

            // 等待调用前已成功入队的任务完成。调用方应先停止新的 Post；超时时返回 false。
            bool WaitForIdle(std::chrono::milliseconds timeout);

            // 请求停止接收新任务，并让工作线程排空已有任务；可从任务或错误回调中调用。
            void RequestStop();

            // 停止接收新任务、排空已有任务并回收工作线程。
            // 从 MongoDB 工作线程调用时不会等待自身，只会请求停止；对象销毁必须由外部线程执行。
            void Stop();

            AsyncMongoDispatcherMetrics Metrics() const noexcept;

        private:
            class Worker;

            void RunWorker(Worker& worker) noexcept;
            std::size_t WorkerIndex(std::int64_t playerId) const noexcept;

            AsyncMongoDispatcherOptions options_;
            ErrorHandler errorHandler_;
            std::unique_ptr<MongoClient> client_;
            std::vector<std::unique_ptr<Worker>> workers_;

            std::atomic<bool> stopping_{false};
            std::atomic<std::uint64_t> posted_{0};
            std::atomic<std::uint64_t> completed_{0};
            std::atomic<std::uint64_t> failed_{0};
            std::atomic<std::uint64_t> rejected_{0};
            std::atomic<std::uint64_t> queued_{0};
            std::atomic<std::uint64_t> active_{0};
            // 已接受但尚未结束的任务数，是 WaitForIdle 的唯一完成判定。
            std::atomic<std::uint64_t> pending_{0};

            mutable std::mutex lifecycleMutex_;
            std::condition_variable lifecycleCondition_;
            bool stopRequested_ = false;
            bool joining_ = false;
            bool stopped_ = false;
            mutable std::mutex idleMutex_;
            std::condition_variable idleCondition_;
    };

} // namespace mongo
