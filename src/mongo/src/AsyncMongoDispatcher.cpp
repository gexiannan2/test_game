#include "AsyncMongoDispatcher.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace mongo
{
    class AsyncMongoDispatcher::Worker final
    {
        public:
            struct TaskItem
            {
                std::int64_t playerId = 0;
                Task task;
            };

            std::mutex mutex;
            std::condition_variable condition;
            std::deque<TaskItem> queue;
            std::thread thread;
            bool stopping = false;
    };

    AsyncMongoDispatcher::AsyncMongoDispatcher(
        MongoConfig config,
        AsyncMongoDispatcherOptions options,
        ErrorHandler errorHandler)
        : options_(options),
          errorHandler_(std::move(errorHandler)),
          client_(std::make_unique<MongoClient>(std::move(config)))
    {
        if (options_.workerCount == 0)
        {
            throw std::invalid_argument("MongoDB 异步工作线程数必须大于 0");
        }
        if (options_.maxQueuedTasksPerWorker == 0)
        {
            throw std::invalid_argument("MongoDB 异步单线程队列上限必须大于 0");
        }

        workers_.reserve(options_.workerCount);
        try
        {
            for (std::size_t index = 0; index < options_.workerCount; ++index)
            {
                auto worker = std::make_unique<Worker>();
                Worker* const workerPointer = worker.get();
                worker->thread = std::thread([this, workerPointer]() {
                    RunWorker(*workerPointer);
                });
                workers_.emplace_back(std::move(worker));
            }
        }
        catch (...)
        {
            Stop();
            throw;
        }
    }

    AsyncMongoDispatcher::~AsyncMongoDispatcher()
    {
        Stop();
    }

    bool AsyncMongoDispatcher::Post(std::int64_t playerId, Task task)
    {
        if (!task)
        {
            throw std::invalid_argument("MongoDB 异步任务不能为空");
        }
        if (stopping_.load(std::memory_order_acquire))
        {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        Worker& worker = *workers_[WorkerIndex(playerId)];
        {
            std::lock_guard<std::mutex> lock(worker.mutex);
            if (worker.stopping || worker.queue.size() >= options_.maxQueuedTasksPerWorker)
            {
                rejected_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            worker.queue.push_back({playerId, std::move(task)});
            posted_.fetch_add(1, std::memory_order_relaxed);
            queued_.fetch_add(1, std::memory_order_relaxed);
            pending_.fetch_add(1, std::memory_order_release);
        }
        worker.condition.notify_one();
        return true;
    }

    bool AsyncMongoDispatcher::WaitForIdle(std::chrono::milliseconds timeout)
    {
        if (timeout.count() < 0)
        {
            throw std::invalid_argument("MongoDB 异步等待超时不能小于 0");
        }
        std::unique_lock<std::mutex> lock(idleMutex_);
        return idleCondition_.wait_for(lock, timeout, [this]() {
            return pending_.load(std::memory_order_acquire) == 0;
        });
    }

    void AsyncMongoDispatcher::RequestStop()
    {
        std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
        if (stopRequested_)
        {
            return;
        }

        stopping_.store(true, std::memory_order_release);
        for (const auto& worker : workers_)
        {
            {
                std::lock_guard<std::mutex> workerLock(worker->mutex);
                worker->stopping = true;
            }
            worker->condition.notify_one();
        }
        stopRequested_ = true;
    }

    void AsyncMongoDispatcher::Stop()
    {
        RequestStop();

        const auto currentThreadId = std::this_thread::get_id();
        for (const auto& worker : workers_)
        {
            if (worker->thread.get_id() == currentThreadId)
            {
                // 工作线程不能 join 自身；外部线程或析构函数稍后负责回收线程对象。
                return;
            }
        }

        std::unique_lock<std::mutex> lifecycleLock(lifecycleMutex_);
        while (joining_)
        {
            lifecycleCondition_.wait(lifecycleLock, [this]() { return stopped_ || !joining_; });
        }
        if (stopped_)
        {
            return;
        }
        joining_ = true;
        lifecycleLock.unlock();

        try
        {
            for (const auto& worker : workers_)
            {
                if (worker->thread.joinable())
                {
                    worker->thread.join();
                }
            }
        }
        catch (...)
        {
            lifecycleLock.lock();
            joining_ = false;
            lifecycleLock.unlock();
            lifecycleCondition_.notify_all();
            throw;
        }

        lifecycleLock.lock();
        stopped_ = true;
        joining_ = false;
        lifecycleLock.unlock();
        lifecycleCondition_.notify_all();
    }

    AsyncMongoDispatcherMetrics AsyncMongoDispatcher::Metrics() const noexcept
    {
        return {
            posted_.load(std::memory_order_relaxed),
            completed_.load(std::memory_order_relaxed),
            failed_.load(std::memory_order_relaxed),
            rejected_.load(std::memory_order_relaxed),
            queued_.load(std::memory_order_relaxed),
            active_.load(std::memory_order_relaxed)};
    }

    void AsyncMongoDispatcher::RunWorker(Worker& worker) noexcept
    {
        while (true)
        {
            Worker::TaskItem item;
            {
                std::unique_lock<std::mutex> lock(worker.mutex);
                worker.condition.wait(lock, [&worker]() {
                    return worker.stopping || !worker.queue.empty();
                });
                if (worker.queue.empty())
                {
                    return;
                }
                item = std::move(worker.queue.front());
                worker.queue.pop_front();
                queued_.fetch_sub(1, std::memory_order_relaxed);
                active_.fetch_add(1, std::memory_order_relaxed);
            }

            try
            {
                item.task(*client_);
                completed_.fetch_add(1, std::memory_order_relaxed);
            }
            catch (...)
            {
                failed_.fetch_add(1, std::memory_order_relaxed);
                if (errorHandler_)
                {
                    try
                    {
                        errorHandler_(item.playerId, std::current_exception());
                    }
                    catch (...)
                    {
                        // 错误回调不能终止 MongoDB 工作线程。
                    }
                }
            }

            active_.fetch_sub(1, std::memory_order_relaxed);
            pending_.fetch_sub(1, std::memory_order_release);
            idleCondition_.notify_all();
        }
    }

    std::size_t AsyncMongoDispatcher::WorkerIndex(std::int64_t playerId) const noexcept
    {
        return static_cast<std::size_t>(static_cast<std::uint64_t>(playerId) % workers_.size());
    }

} // namespace mongo
