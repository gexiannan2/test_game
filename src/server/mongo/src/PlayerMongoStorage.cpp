#include "PlayerMongoStorage.h"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>

#include "google/protobuf/util/json_util.h"

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace mongo
{
    namespace {
        using bsoncxx::builder::basic::document;
        using bsoncxx::builder::basic::kvp;
        using bsoncxx::builder::basic::make_document;

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
