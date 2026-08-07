#include "MongoConfig.h"

#include <climits>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace mongo
{
    namespace {
        std::string ReadEnvironment(const char* name, const std::string& fallback)
        {
#ifdef _WIN32
            char* value = nullptr;
            std::size_t length = 0;
            if (_dupenv_s(&value, &length, name) != 0 || value == nullptr)
            {
                return fallback;
            }
            const std::string result = *value == '\0' ? fallback : value;
            std::free(value);
            return result;
#else
            const char* value = std::getenv(name);
            return value == nullptr || *value == '\0' ? fallback : value;
#endif
        }

        std::int64_t ReadPositiveInt64(const char* name, const std::string& fallback)
        {
            const std::string value = ReadEnvironment(name, fallback);
            try
            {
                const auto result = std::stoll(value);
                if (result <= 0)
                {
                    throw std::invalid_argument("not positive");
                }
                return result;
            }
            catch (const std::exception&)
            {
                throw std::invalid_argument(std::string(name) + " 必须是正整数");
            }
        }

        bool ReadBoolean(const char* name, bool fallback)
        {
            const std::string value = ReadEnvironment(name, fallback ? "true" : "false");
            if (value == "true" || value == "1")
            {
                return true;
            }
            if (value == "false" || value == "0")
            {
                return false;
            }
            throw std::invalid_argument(std::string(name) + " 必须是 true、false、1 或 0");
        }

        std::size_t ReadSize(const char* name, const std::string& fallback)
        {
            const auto value = ReadPositiveInt64(name, fallback);
            if (static_cast<std::uint64_t>(value) >
                static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
            {
                throw std::invalid_argument(std::string(name) + " 超出允许范围");
            }
            return static_cast<std::size_t>(value);
        }

    } // namespace

    MongoConfig MongoConfig::FromEnvironment()
    {
        MongoConfig config;
        config.uri = ReadEnvironment("MONGO_URI", config.uri);
        config.database = ReadEnvironment("MONGO_DATABASE", config.database);
        config.serverSelectionTimeout = std::chrono::milliseconds(
            ReadPositiveInt64("MONGO_SERVER_SELECTION_TIMEOUT_MS", "5000"));
        config.connectTimeout = std::chrono::milliseconds(
            ReadPositiveInt64("MONGO_CONNECT_TIMEOUT_MS", "3000"));
        config.socketTimeout = std::chrono::milliseconds(
            ReadPositiveInt64("MONGO_SOCKET_TIMEOUT_MS", "5000"));
        config.waitQueueTimeout = std::chrono::milliseconds(
            ReadPositiveInt64("MONGO_WAIT_QUEUE_TIMEOUT_MS", "200"));
        config.writeConcernTimeout = std::chrono::milliseconds(
            ReadPositiveInt64("MONGO_WRITE_CONCERN_TIMEOUT_MS", "5000"));
        config.maxPoolSize = ReadSize("MONGO_MAX_POOL_SIZE", "32");
        config.minPoolSize = ReadSize("MONGO_MIN_POOL_SIZE", "4");
        config.maxInFlightRequests = ReadSize("MONGO_MAX_IN_FLIGHT", "64");
        config.writeConcern = ReadEnvironment("MONGO_WRITE_CONCERN", "1");
        config.retryWrites = ReadBoolean("MONGO_RETRY_WRITES", true);
        config.journal = ReadBoolean("MONGO_JOURNAL", false);
        config.tls = ReadBoolean("MONGO_TLS", false);
        config.requireTls = ReadBoolean("MONGO_REQUIRE_TLS", false);
        config.production = ReadEnvironment("MONGO_ENV", "development") == "production";
        config.Validate();
        return config;
    }

    void MongoConfig::Validate() const
    {
        if (uri.empty())
        {
            throw std::invalid_argument("MongoDB URI 不能为空");
        }
        if (database.empty())
        {
            throw std::invalid_argument("MongoDB 数据库名不能为空");
        }
        if (serverSelectionTimeout.count() <= 0 ||
            connectTimeout.count() <= 0 ||
            socketTimeout.count() <= 0 ||
            waitQueueTimeout.count() <= 0 ||
            writeConcernTimeout.count() <= 0)
        {
            throw std::invalid_argument("MongoDB 超时必须大于 0");
        }
        if (maxPoolSize == 0 || minPoolSize > maxPoolSize ||
            maxInFlightRequests == 0 ||
            maxInFlightRequests > static_cast<std::size_t>(INT_MAX))
        {
            throw std::invalid_argument("MongoDB 连接池或并发请求上限配置无效");
        }
        if (writeConcern != "1" && writeConcern != "majority")
        {
            throw std::invalid_argument("MONGO_WRITE_CONCERN 只能是 1 或 majority");
        }
        if (requireTls && !tls)
        {
            throw std::invalid_argument("当前部署策略要求设置 MONGO_TLS=true");
        }
        if (production)
        {
            if (uri.find("directConnection=true") != std::string::npos)
            {
                throw std::invalid_argument("生产环境不能使用 directConnection=true");
            }
            if (uri.find('@') == std::string::npos)
            {
                throw std::invalid_argument("生产环境 MONGO_URI 必须包含认证账号");
            }
            if (writeConcern != "majority" || !journal)
            {
                throw std::invalid_argument(
                    "生产环境必须使用 majority 写关注并设置 MONGO_JOURNAL=true");
            }
        }
    }

    std::string MongoConfig::EffectiveUri() const
    {
        Validate();
        std::string result = uri;
        const char separator = result.find('?') == std::string::npos ? '?' : '&';
        result += separator;
        result += "serverSelectionTimeoutMS=" + std::to_string(serverSelectionTimeout.count());
        result += "&connectTimeoutMS=" + std::to_string(connectTimeout.count());
        result += "&socketTimeoutMS=" + std::to_string(socketTimeout.count());
        result += "&waitQueueTimeoutMS=" + std::to_string(waitQueueTimeout.count());
        result += "&maxPoolSize=" + std::to_string(maxPoolSize);
        result += "&minPoolSize=" + std::to_string(minPoolSize);
        result += "&retryWrites=" + std::string(retryWrites ? "true" : "false");
        result += "&w=" + writeConcern;
        result += "&wTimeoutMS=" + std::to_string(writeConcernTimeout.count());
        result += "&journal=" + std::string(journal ? "true" : "false");
        if (tls)
        {
            result += "&tls=true";
        }
        return result;
    }

} // namespace mongo
