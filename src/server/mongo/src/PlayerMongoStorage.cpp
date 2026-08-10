#include "PlayerMongoStorage.h"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>

#include "google/protobuf/util/json_util.h"

#include <bit>
#include <chrono>
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

        constexpr std::string_view kAccountInfoCollection = "account_info";

        bool IsUtf8Continuation(const unsigned char byte) noexcept
        {
            return byte >= 0x80U && byte <= 0xBFU;
        }

        // account_info 只在首次插入时初始化，登录不能覆盖充值或密码等已有数据。
        void UpsertAccountInfo(MongoClient& client,
                               const AccountInfoSnapshot& snapshot,
                               const std::string& documentId)
        {
            document filter;
            filter.append(kvp("_id", documentId));

            document fields;
            fields.append(
                kvp("account_id", static_cast<std::int64_t>(snapshot.accountId)),
                kvp("channel_id", static_cast<std::int64_t>(snapshot.channelId)),
                kvp("account", snapshot.account),
                kvp("passwd", snapshot.passwd),
                kvp("charge", static_cast<std::int64_t>(snapshot.charge)),
                kvp("area_id", static_cast<std::int64_t>(snapshot.areaId)));
            document update;
            update.append(kvp("$setOnInsert", fields.extract()));

            const auto result = client.UpdateOne(
                kAccountInfoCollection, filter.view(), update.view(), true);
            if (result.matchedCount != 1 && !result.upserted)
            {
                throw std::runtime_error("account_info 没有匹配或写入文档");
            }
        }

        // 把 entity_player_data 整体 MessageToJsonString 为明文 JSON 存入 mongo data 字段：
        //   _id           = base.id（玩家 role_id）
        //   data          = entity_player_data 的 JSON 明文（base + base_data + battle 全字段）
        //   last_sequence = 进程内单调 sequence（用于乱序覆盖可观测）
        //   update_time   = 落地时间（运维用）
        // 统一序列化：proto 全字段自动包含，无需逐字段映射，前向兼容；明文可读便于运维查看。
        void SaveSnapshot(MongoClient& client, const std::string& collection,
                          const ::entity_player_data& data, std::uint64_t sequence)
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

    std::string MakeAccountInfoDocumentId(
        const std::uint32_t channelId,
        const std::int64_t areaId,
        const std::string_view account)
    {
        return std::to_string(channelId) + ":" + std::to_string(areaId) + ":" +
               std::string(account);
    }

    std::int64_t MakeAccountInfoRouteKey(const std::string_view documentId) noexcept
    {
        constexpr std::uint64_t offsetBasis = 14695981039346656037ULL;
        constexpr std::uint64_t prime = 1099511628211ULL;

        std::uint64_t hash = offsetBasis;
        for (const char value : documentId)
        {
            hash ^= static_cast<unsigned char>(value);
            hash *= prime;
        }
        return std::bit_cast<std::int64_t>(hash);
    }

    PlayerMongoStorage::PlayerMongoStorage(
        MongoConfig config,
        PlayerMongoStorageOptions options,
        AsyncMongoDispatcher::ErrorHandler errorHandler)
        : collection_(std::move(options.collection)),
          dispatcher_(std::move(config), options.dispatcher, std::move(errorHandler))
    {
        if (collection_.empty())
        {
            throw std::invalid_argument("玩家 MongoDB 集合名不能为空");
        }
    }

    bool PlayerMongoStorage::PostSave(::entity_player_data data, std::uint64_t sequence,
                                      CompletionHandler completion)
    {
        const std::int64_t playerId = static_cast<std::int64_t>(data.base().id());
        const bool posted = dispatcher_.Post(
            playerId,
            [collection = collection_, data = std::move(data), sequence, completion, playerId](MongoClient& client) {
                try
                {
                    SaveSnapshot(client, collection, data, sequence);
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
            const std::string documentId;
            if (completion)
            {
                completion(
                    false,
                    documentId,
                    std::make_exception_ptr(std::invalid_argument(
                        "account_info 账号必须是 1～20 个合法 UTF-8 字符")));
            }
            return false;
        }

        std::string documentId = MakeAccountInfoDocumentId(
            snapshot.channelId, snapshot.areaId, snapshot.account);
        const std::int64_t routeKey = MakeAccountInfoRouteKey(documentId);
        const bool posted = dispatcher_.Post(
            routeKey,
            [snapshot = std::move(snapshot), documentId, completion](MongoClient& client)
            {
                try
                {
                    UpsertAccountInfo(client, snapshot, documentId);
                    if (completion)
                    {
                        completion(true, documentId, nullptr);
                    }
                }
                catch (...)
                {
                    if (completion)
                    {
                        completion(false, documentId, std::current_exception());
                    }
                    else
                    {
                        throw;  // 无回调时重新抛，走统一 ErrorHandler。
                    }
                }
            });
        if (!posted && completion)
        {
            completion(false, std::move(documentId), nullptr);
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
