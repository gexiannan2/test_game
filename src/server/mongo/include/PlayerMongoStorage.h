#pragma once

#include "AsyncMongoDispatcher.h"

#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <string>

#include "client_common.pb.h"  // entity_player_data（PostSave 载荷）

namespace mongo
{
    struct PlayerMongoStorageOptions
    {
        std::string collection = "players";
        AsyncMongoDispatcherOptions dispatcher;
    };

    // 面向游戏业务的玩家快照异步持久化门面。
    class PlayerMongoStorage final
    {
        public:
            PlayerMongoStorage(
                MongoConfig config,
                PlayerMongoStorageOptions options = {},
                AsyncMongoDispatcher::ErrorHandler errorHandler = {});

            PlayerMongoStorage(const PlayerMongoStorage&) = delete;
            PlayerMongoStorage& operator=(const PlayerMongoStorage&) = delete;
            PlayerMongoStorage(PlayerMongoStorage&&) = delete;
            PlayerMongoStorage& operator=(PlayerMongoStorage&&) = delete;

            // 落地完成回调：success 表示写库是否成功；error 为失败时的异常（可能为空）。
            // 在 mongo worker 线程执行；如需回业务线程，回调内部自行投递。
            using CompletionHandler = std::function<void(bool success, std::int64_t playerId, std::exception_ptr error)>;

            // 传入值语义 protobuf 载荷 + sequence（用于乱序覆盖可观测）；
            // 成功入队返回 true，队列满或停服时返回 false。
            // completion（可选）在 worker 写库完毕后调用；无回调时失败走 ErrorHandler。
            bool PostSave(::entity_player_data data, std::uint64_t sequence, CompletionHandler completion = {});

            // 加载完成回调：success=是否查到存档；data 为存档（无存档时默认构造）。
            // 在 mongo worker 线程执行；如需回业务线程，回调内部自行投递。
            using LoadHandler = std::function<void(bool success, std::int64_t playerId,
                                                    ::entity_player_data data,
                                                    std::exception_ptr error)>;

            // 异步加载玩家存档（QueryRole 底层）：worker 线程 FindOne + JsonStringToMessage。
            // success=true 表示查到且反序列化成功；false 表示无存档（新角色）或失败。
            // 成功入队返回 true，队列满或停服时返回 false。
            bool PostLoad(std::int64_t playerId, LoadHandler completion);

            // 删除完成回调：success=是否删除成功（删除 0 条也算成功）。
            using DeleteHandler = std::function<void(bool success, std::int64_t playerId,
                                                      std::exception_ptr error)>;

            // 异步删除玩家存档：worker 线程 DeleteOne。
            // 成功入队返回 true，队列满或停服时返回 false。
            bool PostDelete(std::int64_t playerId, DeleteHandler completion = {});

            // 异步 ping：在 worker 线程执行 MongoClient::Ping，保持连接池活跃。
            // 成功入队返回 true，队列满或停服时返回 false。
            bool PostPing();

            bool WaitForIdle(std::chrono::milliseconds timeout);
            void RequestStop();
            void Stop();
            AsyncMongoDispatcherMetrics Metrics() const noexcept;

        private:
            std::string collection_;
            AsyncMongoDispatcher dispatcher_;
    };

} // namespace mongo
