#pragma once

#include "AsyncMongoDispatcher.h"

#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <string>
#include <string_view>

#include "client_common.pb.h"  // entity_player_data（PostSave 载荷）

namespace mongo
{
    struct AccountInfoSnapshot
    {
        std::string account;
        std::uint32_t channelId = 0;
        std::int64_t accountId = 0;
        std::string passwd = "0";
        std::int64_t charge = 0;
        std::int64_t areaId = 0;
    };

    // account_info 账号必须是 1～20 个合法 UTF-8 字符。
    bool IsValidAccountInfoAccount(std::string_view account) noexcept;

    // 使用稳定业务键生成 MongoDB 文档 _id；不得改用进程相关哈希值。
    std::string MakeAccountInfoDocumentId(
        std::uint32_t channelId,
        std::int64_t areaId,
        std::string_view account);

    // 仅用于异步 worker 路由；返回值保留标准 FNV-1a 64 位哈希的全部位。
    std::int64_t MakeAccountInfoRouteKey(std::string_view documentId) noexcept;

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

            // 账号信息写入完成回调；入队失败或参数非法时也会在调用线程同步回调失败。
            // 成功入队后在 mongo worker 线程执行；如需回业务线程，回调内部自行投递。
            using AccountInfoCompletionHandler = std::function<void(
                bool success,
                std::string documentId,
                std::exception_ptr error)>;

            // 异步幂等创建 account_info；只使用 $setOnInsert，不覆盖已有业务字段。
            bool PostUpsertAccountInfo(
                AccountInfoSnapshot snapshot,
                AccountInfoCompletionHandler completion = {});

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
