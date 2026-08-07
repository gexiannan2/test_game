#pragma once

#include "AsyncMongoDispatcher.h"
#include "AccountIdGenerator.h"

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

    // account_info 账号必须是 1~20 个合法 UTF-8 字符。
    bool IsValidAccountInfoAccount(std::string_view account) noexcept;

    // account_info 集合默认名（生产固定，可通过 PlayerMongoStorageOptions 覆盖）。
    // 集中定义避免在 .cpp / 测试 / 服务进程里散落硬编码字符串。
    namespace defaults
    {
        constexpr std::string_view kPlayersCollection = "players";
        constexpr std::string_view kAccountInfoCollection = "account_info";
    }  // namespace defaults

    struct PlayerMongoStorageOptions
    {
        // 玩家存档集合（按 role_id 存取 entity_player_data）。
        std::string collection{defaults::kPlayersCollection};
        // 账号信息集合（登录幂等 upsert，按 MAKE_MOD_ID(account_id, area_id) 复合 _id）。
        std::string accountInfoCollection{defaults::kAccountInfoCollection};
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
            // account_id（可选，>0 时写入 players 文档顶层 account_id 字段，
            // 用于"一账号多角色"场景下从 account_id 反查所有 players）。
            // completion（可选）在 worker 写库完毕后调用；无回调时失败走 ErrorHandler。
            bool PostSave(::entity_player_data data, std::uint64_t sequence,
                          std::int64_t account_id = 0,
                          CompletionHandler completion = {});

            // 账号信息写入完成回调；入队失败或参数非法时也会在调用线程同步回调失败。
            // 成功入队后在 mongo worker 线程执行；如需回业务线程，回调内部自行投递。
            // account_id 是 mongo 实际持久化的 account_id：
            //   - 首次插入（upserted）：调用方传入或 GenerateAccountId 生成
            //   - 命中已有文档（matched）：从已有文档 account_id 字段读出
            // 业务侧应将此值回写 AccountComponent.account_id_，供后续 PostSave 落地 players。
            // documentId 是 _id（= account_id * INDEX_MOD_NUM + area_id），便于日志/反解。
            using AccountInfoCompletionHandler = std::function<void(
                bool success,
                std::int64_t account_id,
                std::int64_t documentId,
                std::exception_ptr error)>;

            // 异步幂等 upsert account_info：filter 业务联合键 (channel_id, account, area_id)，
            // 与 idx_login 唯一索引对齐，同一账号重复登录复用同一条文档。
            // 动态字段 (channel_id/account/area_id) 走 $set；_id/account_id/passwd/charge
            // 走 $setOnInsert，仅首次插入落地（不覆盖充值、密码等已有数据）。
            // 若 snapshot.accountId == 0，会在调用线程自动调用 GenerateAccountId() 生成。
            // _id = MakeAccountInfoId(accountId, areaId) = accountId * INDEX_MOD_NUM + areaId，
            // 由 $setOnInsert 在首次插入时锁定，后续 upsert 不变更 _id。
            // areaId 必须 ∈ [0, INDEX_MOD_NUM)，否则同步回调失败。
            // 路由键 = hash(channel_id, account)：同一账号请求由 dispatcher 路由到同一
            // worker 并按 FIFO 串行执行，避免 accountId 自动生成场景下的并发双写。
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

            // 集合名 getter（测试 / 服务进程观测用，不参与持久化逻辑）。
            const std::string& PlayersCollection() const noexcept
            {
                return collection_;
            }

            const std::string& AccountInfoCollection() const noexcept
            {
                return accountInfoCollection_;
            }

        private:
            std::string collection_;
            std::string accountInfoCollection_;
            AsyncMongoDispatcher dispatcher_;
    };

} // namespace mongo
