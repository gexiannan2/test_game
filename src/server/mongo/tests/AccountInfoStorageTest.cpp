#include "MongoClient.h"
#include "PlayerMongoStorage.h"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
    using bsoncxx::builder::basic::document;
    using bsoncxx::builder::basic::kvp;

    void Require(bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    void RunAccountValidationTest()
    {
        Require(!mongo::IsValidAccountInfoAccount(""), "空账号不应通过校验");
        Require(mongo::IsValidAccountInfoAccount(std::string(20, 'a')),
                "20 个 ASCII 字符账号应通过校验");
        Require(!mongo::IsValidAccountInfoAccount(std::string(21, 'a')),
                "21 个 ASCII 字符账号不应通过校验");

        std::string chinese20;
        for (int index = 0; index < 20; ++index)
        {
            chinese20 += "账";
        }
        Require(mongo::IsValidAccountInfoAccount(chinese20),
                "20 个中文字符账号应通过校验");
        chinese20 += "号";
        Require(!mongo::IsValidAccountInfoAccount(chinese20),
                "21 个中文字符账号不应通过校验");

        Require(!mongo::IsValidAccountInfoAccount(std::string("\xC0\xAF", 2)),
                "UTF-8 过长编码不应通过校验");
        Require(!mongo::IsValidAccountInfoAccount(std::string("\xED\xA0\x80", 3)),
                "UTF-8 代理项不应通过校验");
        Require(!mongo::IsValidAccountInfoAccount(std::string("\xF4\x90\x80\x80", 4)),
                "超过 Unicode 上限的编码不应通过校验");
        Require(!mongo::IsValidAccountInfoAccount(std::string("\xE8\xB4", 2)),
                "截断的 UTF-8 编码不应通过校验");

        const auto documentId = mongo::MakeAccountInfoDocumentId(7, 0, "demo");
        Require(documentId == "7:0:demo", "account_info 复合 _id 不正确");
        Require(mongo::MakeAccountInfoRouteKey(documentId) ==
                    mongo::MakeAccountInfoRouteKey(documentId),
                "account_info 路由键必须稳定");
    }

    bool IntegrationEnabled()
    {
        const char* value = std::getenv("MONGO_ACCOUNT_INFO_TEST");
        return value != nullptr && std::string(value) == "1";
    }

    std::string UniqueAccount()
    {
        const auto value = static_cast<unsigned long long>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        return "acct" + std::to_string(value % 1000000000000ULL);
    }

    void RequireAccountDocument(const bsoncxx::document::view& view,
                                const mongo::AccountInfoSnapshot& snapshot)
    {
        Require(view["account_id"].type() == bsoncxx::type::k_int64,
                "account_id 必须是 BSON int64");
        Require(view["channel_id"].type() == bsoncxx::type::k_int64,
                "channel_id 必须是 BSON int64");
        Require(view["account"].type() == bsoncxx::type::k_string,
                "account 必须是 BSON string");
        Require(view["passwd"].type() == bsoncxx::type::k_string,
                "passwd 必须是 BSON string");
        Require(view["charge"].type() == bsoncxx::type::k_int64,
                "charge 必须是 BSON int64");
        Require(view["area_id"].type() == bsoncxx::type::k_int64,
                "area_id 必须是 BSON int64");
        Require(view["account_id"].get_int64().value == snapshot.accountId,
                "account_id 默认值不正确");
        Require(view["channel_id"].get_int64().value ==
                    static_cast<std::int64_t>(snapshot.channelId),
                "channel_id 不正确");
        Require(view["account"].get_string().value == snapshot.account,
                "account 不正确");
        Require(view["passwd"].get_string().value == snapshot.passwd,
                "passwd 默认值不正确");
        Require(view["charge"].get_int64().value == snapshot.charge,
                "charge 默认值不正确");
        Require(view["area_id"].get_int64().value == snapshot.areaId,
                "area_id 默认值不正确");
        Require(view.find("uid") == view.end(), "account_info 不应包含 uid 字段");
    }

    void RunAccountInfoIntegrationTest()
    {
        if (!IntegrationEnabled())
        {
            std::cout << "MongoDB account_info integration test skipped "
                         "(set MONGO_ACCOUNT_INFO_TEST=1 to enable)\n";
            return;
        }

        auto config = mongo::MongoConfig::FromEnvironment();
        Require(config.database.find("test") != std::string::npos,
                "account_info 集成测试只能连接名称包含 test 的数据库");

        mongo::PlayerMongoStorage storage(
            config,
            {.collection = "account_info_player_unused",
             .dispatcher = {.workerCount = 2, .maxQueuedTasksPerWorker = 32}});

        mongo::AccountInfoSnapshot snapshot;
        snapshot.account = UniqueAccount();
        snapshot.channelId = 4000000000U;
        const std::string documentId = mongo::MakeAccountInfoDocumentId(
            snapshot.channelId, snapshot.areaId, snapshot.account);

        std::atomic<bool> callbackCalled{false};
        std::atomic<bool> callbackSuccess{false};
        Require(storage.PostUpsertAccountInfo(
                    snapshot,
                    [&](bool success, std::string id, std::exception_ptr)
                    {
                        callbackSuccess.store(success && id == documentId,
                                              std::memory_order_release);
                        callbackCalled.store(true, std::memory_order_release);
                    }),
                "首次 account_info 任务投递失败");
        Require(storage.WaitForIdle(std::chrono::seconds(10)),
                "首次 account_info 写入没有在超时前完成");
        Require(callbackCalled.load(std::memory_order_acquire) &&
                    callbackSuccess.load(std::memory_order_acquire),
                "首次 account_info 写入回调失败");

        mongo::MongoClient verifier(config);
        document filter;
        filter.append(kvp("_id", documentId));
        auto saved = verifier.FindOne("account_info", filter.view());
        Require(saved.has_value(), "account_info 文档不存在");
        RequireAccountDocument(saved->view(), snapshot);

        document preservedFields;
        preservedFields.append(kvp("account_id", std::int64_t{99}),
                               kvp("passwd", "preserved"),
                               kvp("charge", std::int64_t{12345}));
        document preserveUpdate;
        preserveUpdate.append(kvp("$set", preservedFields.extract()));
        const auto preserveResult = verifier.UpdateOne(
            "account_info", filter.view(), preserveUpdate.view());
        Require(preserveResult.matchedCount == 1,
                "预置 account_info 保留字段失败");

        callbackCalled.store(false, std::memory_order_release);
        callbackSuccess.store(false, std::memory_order_release);
        Require(storage.PostUpsertAccountInfo(
                    snapshot,
                    [&](bool success, std::string id, std::exception_ptr)
                    {
                        callbackSuccess.store(success && id == documentId,
                                              std::memory_order_release);
                        callbackCalled.store(true, std::memory_order_release);
                    }),
                "重复 account_info 任务投递失败");
        Require(storage.WaitForIdle(std::chrono::seconds(10)),
                "重复 account_info 写入没有在超时前完成");
        Require(callbackCalled.load(std::memory_order_acquire) &&
                    callbackSuccess.load(std::memory_order_acquire),
                "重复 account_info 写入回调失败");

        saved = verifier.FindOne("account_info", filter.view());
        Require(saved.has_value(), "重复登录后 account_info 文档不存在");
        const auto preserved = saved->view();
        Require(preserved["account_id"].get_int64().value == 99,
                "重复登录覆盖了 account_id");
        Require(preserved["passwd"].get_string().value == "preserved",
                "重复登录覆盖了 passwd");
        Require(preserved["charge"].get_int64().value == 12345,
                "重复登录覆盖了 charge");

        document businessKeyFilter;
        businessKeyFilter.append(
            kvp("channel_id", static_cast<std::int64_t>(snapshot.channelId)),
            kvp("area_id", static_cast<std::int64_t>(snapshot.areaId)),
            kvp("account", snapshot.account));
        Require(verifier.Count("account_info", businessKeyFilter.view()) == 1,
                "重复登录生成了重复 account_info 文档");

        auto otherChannel = snapshot;
        ++otherChannel.channelId;
        Require(storage.PostUpsertAccountInfo(otherChannel),
                "不同渠道 account_info 任务投递失败");

        auto otherAccount = snapshot;
        otherAccount.account += "x";
        Require(storage.PostUpsertAccountInfo(otherAccount),
                "不同账号 account_info 任务投递失败");

        storage.RequestStop();
        Require(storage.WaitForIdle(std::chrono::seconds(10)),
                "account_info 存储没有在超时前排空");

        callbackCalled.store(false, std::memory_order_release);
        callbackSuccess.store(true, std::memory_order_release);
        Require(!storage.PostUpsertAccountInfo(
                    snapshot,
                    [&](bool success, std::string, std::exception_ptr)
                    {
                        callbackSuccess.store(success, std::memory_order_release);
                        callbackCalled.store(true, std::memory_order_release);
                    }),
                "停服后 account_info 任务不应继续入队");
        Require(callbackCalled.load(std::memory_order_acquire) &&
                    !callbackSuccess.load(std::memory_order_acquire),
                "account_info 队列拒绝没有按失败回调");
        storage.Stop();

        document otherFilter;
        otherFilter.append(kvp(
            "_id", mongo::MakeAccountInfoDocumentId(
                       otherChannel.channelId, otherChannel.areaId,
                       otherChannel.account)));
        Require(verifier.FindOne("account_info", otherFilter.view()).has_value(),
                "不同渠道的同名账号没有生成独立文档");

        document otherAccountFilter;
        otherAccountFilter.append(kvp(
            "_id", mongo::MakeAccountInfoDocumentId(
                       otherAccount.channelId, otherAccount.areaId,
                       otherAccount.account)));
        Require(verifier.FindOne("account_info", otherAccountFilter.view()).has_value(),
                "不同账号没有生成独立 account_info 文档");
    }

    void VerifyLoginWrittenAccountIfRequested()
    {
        const char* expectedAccount = std::getenv("ACCOUNT_INFO_EXPECT_ACCOUNT");
        if (expectedAccount == nullptr || *expectedAccount == '\0')
        {
            return;
        }

        auto config = mongo::MongoConfig::FromEnvironment();
        Require(config.database.find("test") != std::string::npos,
                "登录回读验证只能连接名称包含 test 的数据库");
        const char* channelText = std::getenv("ACCOUNT_INFO_EXPECT_CHANNEL");
        const auto channelId = static_cast<std::uint32_t>(
            channelText == nullptr ? 1ULL : std::stoull(channelText));
        const std::string documentId = mongo::MakeAccountInfoDocumentId(
            channelId, 0, expectedAccount);

        mongo::MongoClient verifier(config);
        document filter;
        filter.append(kvp("_id", documentId));
        const auto saved = verifier.FindOne("account_info", filter.view());
        Require(saved.has_value(), "玩家登录后没有写入 account_info 文档");

        mongo::AccountInfoSnapshot expected;
        expected.account = expectedAccount;
        expected.channelId = channelId;
        RequireAccountDocument(saved->view(), expected);

        document businessKeyFilter;
        businessKeyFilter.append(
            kvp("channel_id", static_cast<std::int64_t>(channelId)),
            kvp("area_id", std::int64_t{0}),
            kvp("account", expectedAccount));
        Require(verifier.Count("account_info", businessKeyFilter.view()) == 1,
                "玩家重复登录生成了重复 account_info 文档");
        std::cout << "verified login-written account_info document: "
                  << documentId << '\n';
    }

    void VerifyAccountAbsentIfRequested()
    {
        const char* expectedAccount = std::getenv("ACCOUNT_INFO_EXPECT_ABSENT");
        if (expectedAccount == nullptr || *expectedAccount == '\0')
        {
            return;
        }

        auto config = mongo::MongoConfig::FromEnvironment();
        Require(config.database.find("test") != std::string::npos,
                "账号未写入验证只能连接名称包含 test 的数据库");
        const char* channelText = std::getenv("ACCOUNT_INFO_EXPECT_CHANNEL");
        const auto channelId = static_cast<std::uint32_t>(
            channelText == nullptr ? 1ULL : std::stoull(channelText));

        mongo::MongoClient verifier(config);
        document filter;
        filter.append(kvp(
            "_id", mongo::MakeAccountInfoDocumentId(channelId, 0, expectedAccount)));
        Require(!verifier.FindOne("account_info", filter.view()).has_value(),
                "非法或超长账号不应写入 account_info");
        std::cout << "verified account_info document absent for account_bytes="
                  << std::string(expectedAccount).size() << '\n';
    }
}

int main()
{
    try
    {
        RunAccountValidationTest();
        RunAccountInfoIntegrationTest();
        VerifyLoginWrittenAccountIfRequested();
        VerifyAccountAbsentIfRequested();
        std::cout << "account_info tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "account_info tests failed: " << error.what() << '\n';
        return 1;
    }
}
