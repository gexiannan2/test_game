#include "MongoClient.h"

#include "MongoError.h"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <mongocxx/exception/exception.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/options/update.hpp>
#include <mongocxx/uri.hpp>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>

namespace mongo
{
    namespace {
        mongocxx::instance& DriverInstance()
        {
            static mongocxx::instance instance;
            return instance;
        }

        std::ptrdiff_t ValidatedRequestLimit(const MongoConfig& config)
        {
            config.Validate();
            return static_cast<std::ptrdiff_t>(config.maxInFlightRequests);
        }

        template <typename Function>
        auto Execute(const char* operation, Function&& function) -> decltype(function())
        {
            try
            {
                return function();
            }
            catch (const MongoError&)
            {
                throw;
            }
            catch (const mongocxx::exception& error)
            {
                throw MongoError(operation, error.code().value(), error.what());
            }
            catch (const std::exception& error)
            {
                throw MongoError(operation, 0, error.what());
            }
        }

        void ValidateCollection(std::string_view name)
        {
            if (name.empty())
            {
                throw std::invalid_argument("MongoDB 集合名不能为空");
            }
        }

    } // namespace

    class MongoClient::RequestGuard final
    {
        public:
            RequestGuard(MongoClient& owner, const char* operation)
                : owner_(&owner)
            {
                owner_->metrics_.submitted.fetch_add(1, std::memory_order_relaxed);
                if (!owner_->requestLimiter_.try_acquire_for(owner_->config_.waitQueueTimeout))
                {
                    owner_->metrics_.rejected.fetch_add(1, std::memory_order_relaxed);
                    owner_ = nullptr;
                    throw MongoError(
                        operation, 0,
                        "MongoDB 客户端背压已触发：并发请求上限或等待超时已达到");
                }

                permitHeld_ = true;
                owner_->metrics_.active.fetch_add(1, std::memory_order_relaxed);
                try
                {
                    entry_.emplace(owner_->pool_->acquire());
                }
                catch (...)
                {
                    owner_->metrics_.failed.fetch_add(1, std::memory_order_relaxed);
                    owner_->metrics_.active.fetch_sub(1, std::memory_order_relaxed);
                    owner_->requestLimiter_.release();
                    permitHeld_ = false;
                    owner_ = nullptr;
                    throw;
                }
            }

            ~RequestGuard()
            {
                if (owner_ == nullptr || !permitHeld_)
                {
                    return;
                }
                if (!succeeded_)
                {
                    owner_->metrics_.failed.fetch_add(1, std::memory_order_relaxed);
                }
                entry_.reset();
                owner_->metrics_.active.fetch_sub(1, std::memory_order_relaxed);
                owner_->requestLimiter_.release();
            }

            RequestGuard(const RequestGuard&) = delete;
            RequestGuard& operator=(const RequestGuard&) = delete;
            RequestGuard(RequestGuard&&) = delete;
            RequestGuard& operator=(RequestGuard&&) = delete;

            mongocxx::client& Client()
            {
                return **entry_;
            }

            void Succeed()
            {
                succeeded_ = true;
                owner_->metrics_.completed.fetch_add(1, std::memory_order_relaxed);
            }

        private:
            MongoClient* owner_ = nullptr;
            std::optional<mongocxx::pool::entry> entry_;
            bool permitHeld_ = false;
            bool succeeded_ = false;
    };

    MongoClient::MongoClient(MongoConfig config)
        : config_(std::move(config)),
          requestLimiter_(ValidatedRequestLimit(config_)),
          pool_(std::make_unique<mongocxx::pool>(
              (DriverInstance(), mongocxx::uri{config_.EffectiveUri()})))
    {
    }

    MongoClient::~MongoClient() = default;

    MongoClient::RequestGuard MongoClient::AcquireRequest(const char* operation)
    {
        try
        {
            return RequestGuard{*this, operation};
        }
        catch (const MongoError&)
        {
            throw;
        }
        catch (const mongocxx::exception& error)
        {
            throw MongoError(operation, error.code().value(), error.what());
        }
        catch (const std::exception& error)
        {
            throw MongoError(operation, 0, error.what());
        }
    }

    void MongoClient::Ping()
    {
        auto request = AcquireRequest("ping");
        Execute("ping", [&]() {
            bsoncxx::builder::basic::document command;
            command.append(bsoncxx::builder::basic::kvp("ping", 1));
            request.Client()[config_.database].run_command(command.view());
        });
        request.Succeed();
    }

    bool MongoClient::InsertOne(
        std::string_view collection,
        bsoncxx::document::view_or_value document)
    {
        ValidateCollection(collection);
        const std::string collectionName{collection};
        auto request = AcquireRequest("insert_one");
        const auto result = Execute("insert_one", [&]() {
            return request.Client()[config_.database][collectionName].insert_one(std::move(document));
        });
        request.Succeed();
        return result.has_value();
    }

    std::optional<bsoncxx::document::value> MongoClient::FindOne(
        std::string_view collection,
        bsoncxx::document::view_or_value filter)
    {
        ValidateCollection(collection);
        const std::string collectionName{collection};
        auto request = AcquireRequest("find_one");
        auto result = Execute("find_one", [&]() {
            return request.Client()[config_.database][collectionName].find_one(std::move(filter));
        });
        request.Succeed();
        return result;
    }

    std::vector<bsoncxx::document::value> MongoClient::Find(
        std::string_view collection,
        bsoncxx::document::view_or_value filter)
    {
        ValidateCollection(collection);
        const std::string collectionName{collection};
        auto request = AcquireRequest("find");
        auto documents = Execute("find", [&]() {
            std::vector<bsoncxx::document::value> result;
            auto cursor = request.Client()[config_.database][collectionName].find(std::move(filter));
            for (const auto& document : cursor)
            {
                result.emplace_back(document);
            }
            return result;
        });
        request.Succeed();
        return documents;
    }

    UpdateResult MongoClient::UpdateOne(
        std::string_view collection,
        bsoncxx::document::view_or_value filter,
        bsoncxx::document::view_or_value update,
        bool upsert)
    {
        ValidateCollection(collection);
        const std::string collectionName{collection};
        auto request = AcquireRequest("update_one");
        const auto result = Execute("update_one", [&]() {
            mongocxx::options::update options;
            options.upsert(upsert);
            return request.Client()[config_.database][collectionName].update_one(
                std::move(filter), std::move(update), options);
        });
        request.Succeed();
        if (!result)
        {
            return {};
        }
        return {
            static_cast<std::int64_t>(result->matched_count()),
            static_cast<std::int64_t>(result->modified_count()),
            result->upserted_id().has_value()};
    }

    UpdateResult MongoClient::UpdateMany(
        std::string_view collection,
        bsoncxx::document::view_or_value filter,
        bsoncxx::document::view_or_value update,
        bool upsert)
    {
        ValidateCollection(collection);
        const std::string collectionName{collection};
        auto request = AcquireRequest("update_many");
        const auto result = Execute("update_many", [&]() {
            mongocxx::options::update options;
            options.upsert(upsert);
            return request.Client()[config_.database][collectionName].update_many(
                std::move(filter), std::move(update), options);
        });
        request.Succeed();
        if (!result)
        {
            return {};
        }
        return {
            static_cast<std::int64_t>(result->matched_count()),
            static_cast<std::int64_t>(result->modified_count()),
            result->upserted_id().has_value()};
    }

    DeleteResult MongoClient::DeleteOne(
        std::string_view collection,
        bsoncxx::document::view_or_value filter)
    {
        ValidateCollection(collection);
        const std::string collectionName{collection};
        auto request = AcquireRequest("delete_one");
        const auto result = Execute("delete_one", [&]() {
            return request.Client()[config_.database][collectionName].delete_one(std::move(filter));
        });
        request.Succeed();
        return {result ? static_cast<std::int64_t>(result->deleted_count()) : 0};
    }

    DeleteResult MongoClient::DeleteMany(
        std::string_view collection,
        bsoncxx::document::view_or_value filter)
    {
        ValidateCollection(collection);
        const std::string collectionName{collection};
        auto request = AcquireRequest("delete_many");
        const auto result = Execute("delete_many", [&]() {
            return request.Client()[config_.database][collectionName].delete_many(std::move(filter));
        });
        request.Succeed();
        return {result ? static_cast<std::int64_t>(result->deleted_count()) : 0};
    }

    std::int64_t MongoClient::Count(
        std::string_view collection,
        bsoncxx::document::view_or_value filter)
    {
        ValidateCollection(collection);
        const std::string collectionName{collection};
        auto request = AcquireRequest("count_documents");
        const auto result = Execute("count_documents", [&]() {
            return request.Client()[config_.database][collectionName].count_documents(std::move(filter));
        });
        request.Succeed();
        return static_cast<std::int64_t>(result);
    }

    MongoClientMetrics MongoClient::Metrics() const noexcept
    {
        return {
            metrics_.submitted.load(std::memory_order_relaxed),
            metrics_.completed.load(std::memory_order_relaxed),
            metrics_.failed.load(std::memory_order_relaxed),
            metrics_.rejected.load(std::memory_order_relaxed),
            metrics_.active.load(std::memory_order_relaxed)};
    }

} // namespace mongo
