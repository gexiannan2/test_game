#include "PlayerMongoStorage.h"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>

#include "google/protobuf/util/json_util.h"

#include "AccountIdGenerator.h"  // mongo::GenerateAccountId, MakeAccountInfoId, IsValidAreaId

#include <chrono>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace mongo
{
    namespace {
        using bsoncxx::builder::basic::document;
        using bsoncxx::builder::basic::kvp;
        using bsoncxx::builder::basic::make_document;

        bool IsUtf8Continuation(const unsigned char byte) noexcept
        {
            return byte >= 0x80U && byte <= 0xBFU;
        }

        // account_info 的 dispatcher 路由键：(channel_id, account) → int64。
        // 用业务主键而非 documentId 作路由键，确保同一账号每次登录
        // 都进入同一 worker（FIFO 顺序保证幂等 upsert）。
        // 同一 channel_id 下不同 account 哈希冲突只会落到同一 worker，
        // 不影响正确性，仅略降低并发吞吐。
        std::int64_t MakeAccountRouteKey(std::uint32_t channelId,
                                          const std::string& account) noexcept
        {
            std::hash<std::string> hasher;
            const std::size_t h = hasher(std::to_string(channelId) + "|" + account);
            return static_cast<std::int64_t>(h);
        }

        // 业务联合键 upsert：filter 用 (channel_id, account, area_id)，
        // 与现有 idx_login 唯一索引对齐，确保同一账号再次登录复用同一条文档。
        // 动态字段（channel_id / account / area_id）走 $set；
        // _id、account_id、passwd、charge 走 $setOnInsert，仅首次插入落地。
        // collection 由调用方传入（来自 PlayerMongoStorage::accountInfoCollection_）。
        // documentId 即 _id：account_id * INDEX_MOD_NUM + area_id（int64）。
        // out_account_id 返回 mongo 实际持久化的 account_id：
        //   - upserted=true（首次插入）：调用方传入（或 GenerateAccountId 生成）的 accountId
        //   - matched=1（命中已有文档）：FindOne 读出已有 account_id 字段，
        //     避免调用方传入的新生成 accountId 与 mongo 实际值不一致
        void UpsertAccountInfo(MongoClient& client,
                               const AccountInfoSnapshot& snapshot,
                               std::int64_t documentId,
                               const std::string& collection,
                               std::int64_t& out_account_id)
        {
            document filter;
            filter.append(
                kvp("channel_id", static_cast<std::int64_t>(snapshot.channelId)),
                kvp("account", snapshot.account),
                kvp("area_id", static_cast<std::int64_t>(snapshot.areaId)));

            document setFields;
            setFields.append(
                kvp("channel_id", static_cast<std::int64_t>(snapshot.channelId)),
                kvp("account", snapshot.account),
                kvp("area_id", static_cast<std::int64_t>(snapshot.areaId)));

            document setOnInsertFields;
            setOnInsertFields.append(
                kvp("_id", documentId),
                kvp("account_id", static_cast<std::int64_t>(snapshot.accountId)),
                kvp("passwd", snapshot.passwd),
                kvp("charge", static_cast<std::int64_t>(snapshot.charge)));

            document update;
            update.append(
                kvp("$set", setFields.extract()),
                kvp("$setOnInsert", setOnInsertFields.extract()));

            const auto result = client.UpdateOne(
                collection, filter.view(), update.view(), true);
            if (result.matchedCount != 1 && !result.upserted)
            {
                throw std::runtime_error("account_info 没有匹配或写入文档");
            }

            // 返回 mongo 实际持久化的 account_id
            if (result.upserted)
            {
                out_account_id = snapshot.accountId;
            }
            else
            {
                // matched=1：读已有文档的 account_id 字段
                const auto doc = client.FindOne(collection, filter.view());
                if (doc)
                {
                    const auto view   = doc->view();
                    const auto accElem = view["account_id"];
                    if (accElem.type() == bsoncxx::type::k_int64)
                    {
                        out_account_id = accElem.get_int64().value;
                    }
                }
                // 兜底：已有文档缺 account_id 字段（异常数据）时从 _id 反解
                if (out_account_id == 0 && doc)
                {
                    const auto idElem = doc->view()["_id"];
                    if (idElem.type() == bsoncxx::type::k_int64)
                    {
                        out_account_id = GetAccountIdFromId(idElem.get_int64().value);
                    }
                }
            }
        }

        // 把 entity_player_data 整体 MessageToJsonString 为明文 JSON 存入 mongo data 字段：
        //   _id           = base.id（玩家 role_id）
        //   data          = entity_player_data 的 JSON 明文（base + base_data + battle 全字段）
        //   last_sequence = 进程内单调 sequence（用于乱序覆盖可观测）
        //   update_time   = 落地时间（运维用）
        //   account_id    = 顶层 account_id 字段（account_id > 0 时写入），
        //                   用于"一账号多角色"场景下从 account_id 反查所有 players
        // 统一序列化：proto 全字段自动包含，无需逐字段映射，前向兼容；明文可读便于运维查看。
        void SaveSnapshot(MongoClient& client, const std::string& collection,
                          const ::entity_player_data& data, std::uint64_t sequence,
                          std::int64_t account_id)
        {
            std::string json_str;
            const auto status = google::protobuf::util::MessageToJsonString(data, &json_str);
            if (!status.ok())
            {
                throw std::runtime_error("entity_player_data MessageToJsonString failed");
            }

            const std::int64_t playerId = static_cast<std::int64_t>(data.base().id());

            document filter;
            filter.append(kvp("_id", playerId));

            document fields;
            fields.append(kvp("data", json_str),
                          kvp("last_sequence", static_cast<std::int64_t>(sequence)),
                          kvp("update_time", bsoncxx::types::b_date{std::chrono::system_clock::now()}));
            // 顶层 account_id：account_id == 0 时不写入（业务侧尚未从 mongo 拿到，等下次落地再写）
            if (account_id != 0)
            {
                fields.append(kvp("account_id", account_id));
            }
            document update;
            update.append(kvp("$set", fields.extract()));

            const auto result = client.UpdateOne(collection, filter.view(), update.view(), true);
            if (result.matchedCount != 1 && !result.upserted)
            {
                throw std::runtime_error("玩家异步快照没有匹配或写入文档");
            }
        }

        // 从 mongo 加载玩家存档：FindOne + JsonStringToMessage 反序列化 data 字段（JSON 明文）。
        // 返回 nullopt 表示无存档（新角色）；有存档但反序列化失败抛异常。
        // 旧明文 BSON 格式（base/base_data/battle 子文档）暂不支持，需先落地迁移为新 JSON 格式。
        std::optional<::entity_player_data> LoadSnapshot(MongoClient& client,
                                                          const std::string& collection,
                                                          std::int64_t playerId)
        {
            document filter;
            filter.append(kvp("_id", playerId));
            const auto doc = client.FindOne(collection, filter.view());
            if (!doc)
            {
                return std::nullopt;  // 新角色无存档
            }
            const auto view = doc->view();
            const auto dataElem = view["data"];
            if (dataElem.type() != bsoncxx::type::k_string)
            {
                // 旧明文 BSON 格式或异常文档：视为无存档（调用方走默认出生点）
                return std::nullopt;
            }
            ::entity_player_data data;
            const auto status = google::protobuf::util::JsonStringToMessage(
                std::string(dataElem.get_string().value), &data);
            if (!status.ok())
            {
                throw std::runtime_error("entity_player_data JsonStringToMessage failed");
            }
            return data;
        }

        // 从 mongo 删除玩家存档：DeleteOne by _id。
        void DeleteSnapshot(MongoClient& client, const std::string& collection,
                            std::int64_t playerId)
        {
            document filter;
            filter.append(kvp("_id", playerId));
            client.DeleteOne(collection, filter.view());
        }

    } // namespace

    bool IsValidAccountInfoAccount(const std::string_view account) noexcept
    {
        if (account.empty())
        {
            return false;
        }

        std::size_t offset = 0;
        std::size_t characterCount = 0;
        while (offset < account.size())
        {
            const auto first = static_cast<unsigned char>(account[offset]);
            std::size_t length = 0;
            if (first <= 0x7FU)
            {
                length = 1;
            }
            else if (first >= 0xC2U && first <= 0xDFU)
            {
                length = 2;
            }
            else if (first >= 0xE0U && first <= 0xEFU)
            {
                length = 3;
            }
            else if (first >= 0xF0U && first <= 0xF4U)
            {
                length = 4;
            }
            else
            {
                return false;
            }

            if (length > account.size() - offset)
            {
                return false;
            }
            for (std::size_t index = 1; index < length; ++index)
            {
                if (!IsUtf8Continuation(static_cast<unsigned char>(account[offset + index])))
                {
                    return false;
                }
            }

            if (length == 3)
            {
                const auto second = static_cast<unsigned char>(account[offset + 1]);
                if ((first == 0xE0U && second < 0xA0U) ||
                    (first == 0xEDU && second > 0x9FU))
                {
                    return false;  // 拒绝过长编码和 UTF-16 代理项。
                }
            }
            else if (length == 4)
            {
                const auto second = static_cast<unsigned char>(account[offset + 1]);
                if ((first == 0xF0U && second < 0x90U) ||
                    (first == 0xF4U && second > 0x8FU))
                {
                    return false;  // Unicode 码点必须位于 U+0000～U+10FFFF。
                }
            }

            offset += length;
            ++characterCount;
            if (characterCount > 20)
            {
                return false;
            }
        }
        return true;
    }

    PlayerMongoStorage::PlayerMongoStorage(
        MongoConfig config,
        PlayerMongoStorageOptions options,
        AsyncMongoDispatcher::ErrorHandler errorHandler)
        : collection_(std::move(options.collection)),
          accountInfoCollection_(std::move(options.accountInfoCollection)),
          dispatcher_(std::move(config), options.dispatcher, std::move(errorHandler))
    {
        if (collection_.empty())
        {
            throw std::invalid_argument("玩家 MongoDB 集合名不能为空");
        }
        if (accountInfoCollection_.empty())
        {
            throw std::invalid_argument("account_info MongoDB 集合名不能为空");
        }
    }

    bool PlayerMongoStorage::PostSave(::entity_player_data data, std::uint64_t sequence,
                                      std::int64_t account_id,
                                      CompletionHandler completion)
    {
        const std::int64_t playerId = static_cast<std::int64_t>(data.base().id());
        const bool posted = dispatcher_.Post(
            playerId,
            [collection = collection_, data = std::move(data), sequence, account_id, completion, playerId](MongoClient& client) {
                try
                {
                    SaveSnapshot(client, collection, data, sequence, account_id);
                    if (completion)
                    {
                        completion(true, playerId, nullptr);
                    }
                }
                catch (...)
                {
                    if (completion)
                    {
                        completion(false, playerId, std::current_exception());
                    }
                    else
                    {
                        throw;  // 无回调时重新抛，走 ErrorHandler
                    }
                }
            });
        if (!posted && completion)
        {
            // 入队失败也必须回调，避免调用方永久挂起
            completion(false, playerId, nullptr);
        }
        return posted;
    }

    bool PlayerMongoStorage::PostUpsertAccountInfo(
        AccountInfoSnapshot snapshot,
        AccountInfoCompletionHandler completion)
    {
        if (!IsValidAccountInfoAccount(snapshot.account))
        {
            if (completion)
            {
                completion(
                    false,
                    0,
                    0,
                    std::make_exception_ptr(std::invalid_argument(
                        "account_info 账号必须是 1～20 个合法 UTF-8 字符")));
            }
            return false;
        }

        // 校验 area_id 范围：必须 ∈ [0, INDEX_MOD_NUM)，否则 _id 不可逆或越界。
        if (!IsValidAreaId(snapshot.areaId))
        {
            if (completion)
            {
                completion(
                    false,
                    0,
                    0,
                    std::make_exception_ptr(std::invalid_argument(
                        "account_info area_id 必须 ∈ [0, 1000000)")));
            }
            return false;
        }

        // 调用方未传 accountId（== 0）时自动生成。
        // 在业务线程生成而非 worker 线程：调用方可立即拿到 accountId 用于日志/回包。
        // 幂等性由 UpsertAccountInfo 内部 $setOnInsert 保证：同一账号多次登录，
        // 只有首次插入时写入 account_id，后续 upsert 不会覆盖。
        // 若命中已有文档，worker 会 FindOne 读出实际 account_id 回调返回，
        // 覆盖此处生成的新值（保证业务侧 AccountComponent.account_id_ 与 mongo 一致）。
        if (snapshot.accountId == 0)
        {
            snapshot.accountId = static_cast<std::int64_t>(mongo::GenerateAccountId());
        }

        // _id = account_id * INDEX_MOD_NUM + area_id（int64 数字主键）
        // 反解：account_id = _id / INDEX_MOD_NUM；area_id = _id % INDEX_MOD_NUM
        const std::int64_t documentId = MakeAccountInfoId(
            snapshot.accountId, snapshot.areaId);
        // 用 (channel_id, account) 作 worker 路由键：同一账号登录请求路由到同一 worker，
        // 由 dispatcher FIFO 保证幂等 upsert 串行，避免 accountId 每次新生成时 _id 漂移导致双写。
        const std::int64_t routeKey = MakeAccountRouteKey(
            snapshot.channelId, snapshot.account);
        const std::string collection = accountInfoCollection_;
        const bool posted = dispatcher_.Post(
            routeKey,
            [snapshot = std::move(snapshot), documentId, collection, completion](MongoClient& client)
            {
                try
                {
                    std::int64_t actual_account_id = 0;
                    UpsertAccountInfo(client, snapshot, documentId, collection, actual_account_id);
                    // 用 actual_account_id 反算 mongo 实际 _id，
                    // 保证回调返回的 documentId 与 mongo 文档 _id 一致
                    const std::int64_t actual_document_id = MakeAccountInfoId(
                        actual_account_id, snapshot.areaId);
                    if (completion)
                    {
                        completion(true, actual_account_id, actual_document_id, nullptr);
                    }
                }
                catch (...)
                {
                    if (completion)
                    {
                        completion(false, 0, documentId, std::current_exception());
                    }
                    else
                    {
                        throw;  // 无回调时重新抛，走统一 ErrorHandler。
                    }
                }
            });
        if (!posted && completion)
        {
            completion(false, 0, documentId, nullptr);
        }
        return posted;
    }

    bool PlayerMongoStorage::PostPing()
    {
        // 用固定 playerId=0 投递到 worker 线程，执行 MongoClient::Ping。
        // 不阻塞业务线程；失败走 ErrorHandler。
        return dispatcher_.Post(
            0,
            [](MongoClient& client) {
                client.Ping();
            });
    }

    bool PlayerMongoStorage::PostLoad(std::int64_t playerId, LoadHandler completion)
    {
        const bool posted = dispatcher_.Post(
            playerId,
            [collection = collection_, playerId, completion](MongoClient& client)
            {
                bool success = false;
                ::entity_player_data data;
                std::exception_ptr ep;
                try
                {
                    auto result = LoadSnapshot(client, collection, playerId);
                    if (result)
                    {
                        success = true;
                        data = std::move(*result);
                    }
                    else
                    {
                        success = false;  // 无存档（新角色）
                    }
                }
                catch (...)
                {
                    ep = std::current_exception();
                    success = false;
                }
                if (completion)
                {
                    completion(success, playerId, std::move(data), ep);
                }
            });
        if (!posted && completion)
        {
            completion(false, playerId, {}, nullptr);
        }
        return posted;
    }

    bool PlayerMongoStorage::PostDelete(std::int64_t playerId, DeleteHandler completion)
    {
        const bool posted = dispatcher_.Post(
            playerId,
            [collection = collection_, playerId, completion](MongoClient& client)
            {
                std::exception_ptr ep;
                bool success = false;
                try
                {
                    DeleteSnapshot(client, collection, playerId);
                    success = true;
                }
                catch (...)
                {
                    ep = std::current_exception();
                    success = false;
                }
                if (completion)
                {
                    completion(success, playerId, ep);
                }
            });
        if (!posted && completion)
        {
            completion(false, playerId, nullptr);
        }
        return posted;
    }

    bool PlayerMongoStorage::WaitForIdle(std::chrono::milliseconds timeout)
    {
        return dispatcher_.WaitForIdle(timeout);
    }

    void PlayerMongoStorage::RequestStop()
    {
        dispatcher_.RequestStop();
    }

    void PlayerMongoStorage::Stop()
    {
        dispatcher_.Stop();
    }

    AsyncMongoDispatcherMetrics PlayerMongoStorage::Metrics() const noexcept
    {
        return dispatcher_.Metrics();
    }

} // namespace mongo
