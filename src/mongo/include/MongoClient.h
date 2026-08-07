#pragma once

#include "MongoConfig.h"

#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view_or_value.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/pool.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <semaphore>
#include <string>
#include <string_view>
#include <vector>

namespace mongo
{
    struct UpdateResult
    {
        std::int64_t matchedCount = 0;
        std::int64_t modifiedCount = 0;
        bool upserted = false;
    };

    struct DeleteResult
    {
        std::int64_t deletedCount = 0;
    };

    struct MongoClientMetrics
    {
        std::uint64_t submitted = 0;
        std::uint64_t completed = 0;
        std::uint64_t failed = 0;
        std::uint64_t rejected = 0;
        std::uint64_t active = 0;
    };

    class MongoClient final
    {
        public:
            explicit MongoClient(MongoConfig config);
            ~MongoClient();

            MongoClient(const MongoClient&) = delete;
            MongoClient& operator=(const MongoClient&) = delete;
            MongoClient(MongoClient&&) = delete;
            MongoClient& operator=(MongoClient&&) = delete;

            void Ping();
            bool InsertOne(std::string_view collection, bsoncxx::document::view_or_value document);
            std::optional<bsoncxx::document::value> FindOne(
                std::string_view collection,
                bsoncxx::document::view_or_value filter);
            std::vector<bsoncxx::document::value> Find(
                std::string_view collection,
                bsoncxx::document::view_or_value filter);
            UpdateResult UpdateOne(
                std::string_view collection,
                bsoncxx::document::view_or_value filter,
                bsoncxx::document::view_or_value update,
                bool upsert = false);
            UpdateResult UpdateMany(
                std::string_view collection,
                bsoncxx::document::view_or_value filter,
                bsoncxx::document::view_or_value update,
                bool upsert = false);
            DeleteResult DeleteOne(
                std::string_view collection,
                bsoncxx::document::view_or_value filter);
            DeleteResult DeleteMany(
                std::string_view collection,
                bsoncxx::document::view_or_value filter);
            std::int64_t Count(
                std::string_view collection,
                bsoncxx::document::view_or_value filter);

            // 创建索引（幂等）。
            //   collection  集合名
            //   keys        索引字段定义，例如 make_document(kvp("a", 1), kvp("b", -1))
            //   unique      是否唯一索引
            //   name        索引名（可空，由驱动自动生成）
            //   sparse      是否稀疏索引（跳过字段缺失的文档）
            //   background  是否后台构建（不阻塞集合读写）
            // 失败抛 MongoError；服务进程启动时调用一次即可。
            void CreateIndex(
                std::string_view collection,
                bsoncxx::document::view_or_value keys,
                bool unique = false,
                std::string_view name = {},
                bool sparse = false,
                bool background = true);

            MongoClientMetrics Metrics() const noexcept;

        private:
            class RequestGuard;
            RequestGuard AcquireRequest(const char* operation);

            struct MetricCounters
            {
                std::atomic<std::uint64_t> submitted{0};
                std::atomic<std::uint64_t> completed{0};
                std::atomic<std::uint64_t> failed{0};
                std::atomic<std::uint64_t> rejected{0};
                std::atomic<std::uint64_t> active{0};
            };

            MongoConfig config_;
            std::counting_semaphore<2147483647> requestLimiter_;
            std::unique_ptr<mongocxx::pool> pool_;
            MetricCounters metrics_;
    };

} // namespace mongo
