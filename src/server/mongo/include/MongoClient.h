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
