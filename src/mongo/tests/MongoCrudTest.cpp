#include "MongoClient.h"
#include "MongoConfig.h"
#include "MongoError.h"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>

#include <atomic>
#include <chrono>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using bsoncxx::builder::basic::document;
using bsoncxx::builder::basic::kvp;

void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

std::string BatchId()
{
    return "mongo_test_" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
}

void RunCrudTest()
{
    auto config = mongo::MongoConfig::FromEnvironment();
    mongo::MongoClient client(config);
    client.Ping();

    const std::string batch = BatchId();
    document batchFilter;
    batchFilter.append(kvp("batch", batch));

    for (int index = 1; index <= 3; ++index)
    {
        document value;
        value.append(
            kvp("_id", batch + "_" + std::to_string(index)),
            kvp("batch", batch),
            kvp("index", index),
            kvp("name", index == 1 ? "中文读写" : "MongoStandalone"),
            kvp("enabled", true));
        Require(client.InsertOne("crud_cases", value.view()), "insert_one 失败");
    }

    Require(client.Count("crud_cases", batchFilter.view()) == 3, "插入数量不正确");

    document firstFilter;
    firstFilter.append(kvp("_id", batch + "_1"));
    const auto first = client.FindOne("crud_cases", firstFilter.view());
    Require(first.has_value(), "find_one 未找到文档");
    Require(first->view()["name"].get_string().value == "中文读写", "UTF-8 字符串不一致");

    document setFields;
    setFields.append(kvp("name", "updated"));
    document incrementFields;
    incrementFields.append(kvp("index", 10));
    document update;
    update.append(kvp("$set", setFields.extract()), kvp("$inc", incrementFields.extract()));
    const auto updateResult =
        client.UpdateOne("crud_cases", firstFilter.view(), update.view());
    Require(updateResult.matchedCount == 1, "update_one 匹配数量不正确");
    Require(updateResult.modifiedCount == 1, "update_one 修改数量不正确");

    const auto updated = client.FindOne("crud_cases", firstFilter.view());
    Require(updated.has_value(), "更新后文档不存在");
    Require(updated->view()["index"].get_int32().value == 11, "$inc 结果不正确");

    document manySet;
    manySet.append(kvp("verified", true));
    document manyUpdate;
    manyUpdate.append(kvp("$set", manySet.extract()));
    const auto manyResult =
        client.UpdateMany("crud_cases", batchFilter.view(), manyUpdate.view());
    Require(manyResult.matchedCount == 3, "update_many 匹配数量不正确");
    Require(manyResult.modifiedCount == 3, "update_many 修改数量不正确");
    Require(client.Find("crud_cases", batchFilter.view()).size() == 3, "find 数量不正确");

    bool duplicateRejected = false;
    try
    {
        document duplicate;
        duplicate.append(kvp("_id", batch + "_1"), kvp("batch", batch));
        client.InsertOne("crud_cases", duplicate.view());
    }
    catch (const mongo::MongoError&)
    {
        duplicateRejected = true;
    }
    Require(duplicateRejected, "重复 _id 没有返回错误");

    Require(
        client.DeleteMany("crud_cases", batchFilter.view()).deletedCount == 3,
        "delete_many 删除数量不正确");
    Require(client.Count("crud_cases", batchFilter.view()) == 0, "测试数据清理失败");
}

void RunConcurrentPoolTest()
{
    auto config = mongo::MongoConfig::FromEnvironment();
    config.minPoolSize = 1;
    config.maxPoolSize = 8;
    config.maxInFlightRequests = 8;
    config.waitQueueTimeout = std::chrono::milliseconds(3000);
    mongo::MongoClient client(config);
    client.Ping();

    const std::string batch = BatchId();
    constexpr int kWorkers = 8;
    constexpr int kInsertsPerWorker = 20;
    std::exception_ptr firstError;
    std::mutex errorMutex;
    std::vector<std::thread> workers;
    workers.reserve(kWorkers);

    for (int worker = 0; worker < kWorkers; ++worker)
    {
        workers.emplace_back([&, worker]() {
            try
            {
                for (int index = 0; index < kInsertsPerWorker; ++index)
                {
                    document value;
                    value.append(
                        kvp("_id", batch + "_" + std::to_string(worker) + "_" +
                            std::to_string(index)),
                        kvp("batch", batch),
                        kvp("worker", worker),
                        kvp("index", index));
                    Require(client.InsertOne("crud_concurrent_cases", value.view()),
                            "并发 insert_one 失败");
                }
            }
            catch (...)
            {
                std::lock_guard<std::mutex> lock(errorMutex);
                if (!firstError)
                {
                    firstError = std::current_exception();
                }
            }
        });
    }
    for (auto& worker : workers)
    {
        worker.join();
    }
    if (firstError)
    {
        std::rethrow_exception(firstError);
    }

    document batchFilter;
    batchFilter.append(kvp("batch", batch));
    Require(client.Count("crud_concurrent_cases", batchFilter.view()) ==
                kWorkers * kInsertsPerWorker,
            "连接池并发插入数量不正确");
    const auto metrics = client.Metrics();
    Require(metrics.failed == 0 && metrics.rejected == 0 && metrics.active == 0,
            "连接池并发请求出现失败、背压拒绝或泄漏");
    Require(metrics.completed >= static_cast<std::uint64_t>(kWorkers * kInsertsPerWorker),
            "连接池并发请求完成计数不正确");
    Require(client.DeleteMany("crud_concurrent_cases", batchFilter.view()).deletedCount ==
                kWorkers * kInsertsPerWorker,
            "连接池并发测试数据清理失败");
}

} // namespace

int main()
{
    try
    {
        RunCrudTest();
        RunConcurrentPoolTest();
        std::cout << "MongoDB CRUD integration test passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "MongoDB CRUD integration test failed: " << error.what() << '\n';
        return 1;
    }
}
