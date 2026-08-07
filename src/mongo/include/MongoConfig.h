#pragma once

#include <chrono>
#include <cstddef>
#include <string>

namespace mongo
{
    struct MongoConfig
    {
        // 本地开发默认连接；生产环境必须由 MONGO_URI 提供副本集 URI。
        std::string uri = "mongodb://127.0.0.1:27017/?directConnection=true";
        std::string database = "game";
        std::chrono::milliseconds serverSelectionTimeout{5000};
        std::chrono::milliseconds connectTimeout{3000};
        std::chrono::milliseconds socketTimeout{5000};
        std::chrono::milliseconds waitQueueTimeout{200};
        std::chrono::milliseconds writeConcernTimeout{5000};
        std::size_t maxPoolSize = 32;
        std::size_t minPoolSize = 4;
        std::size_t maxInFlightRequests = 64;
        std::string writeConcern = "1";
        bool retryWrites = true;
        bool journal = false;
        // 内网默认不使用 TLS；仅在部署边界明确要求时设置为 true。
        bool tls = false;
        // 由部署策略决定是否强制 TLS，而不是由生产环境标签隐式决定。
        bool requireTls = false;
        bool production = false;

        static MongoConfig FromEnvironment();
        void Validate() const;
        std::string EffectiveUri() const;
    };

} // namespace mongo
