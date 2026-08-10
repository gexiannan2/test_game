#include "MongoClient.h"
#include "PlayerMongoStorage.h"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>

namespace mongo
{
    // 在 account_info 集合上建立必要索引。
    // 幂等：已存在的同名索引会被跳过（驱动返回 OK）。
    // 服务进程启动时调用一次即可。
    //
    // 索引设计：
    //   _id 默认主键（int64）：account_id * INDEX_MOD_NUM + area_id
    //     - 覆盖按 (account_id, area_id) 范围查询同账号跨区服文档
    //
    //   idx_login (channel_id, account) 唯一
    //     - 登录请求只带 channel_id + account（玩家还没选区服），无 account_id
    //     - 必须独立索引；同时唯一性约束防止同账号在同渠道重复创建
    //
    //   idx_account_area (account_id, area_id) 非唯一
    //     - 运维/合服脚本按 account_id 列出所有区服文档时使用
    //     - 与 _id 范围查询能力重叠，但显式索引更便于排查和统计
    //
    //   idx_charge (charge 倒序, sparse)
    //     - 充值排行/反作弊查询
    //     - sparse 跳过 charge=0 的大量免费玩家
    void EnsureAccountInfoIndexes(MongoClient& client)
    {
        using bsoncxx::builder::basic::make_document;
        using bsoncxx::builder::basic::kvp;

        // 1. 登录验证唯一索引（最高频，必须）
        client.CreateIndex(
            defaults::kAccountInfoCollection,
            make_document(kvp("channel_id", 1), kvp("account", 1)),
            /*unique=*/true,
            /*name=*/"idx_login",
            /*sparse=*/false,
            /*background=*/true);

        // 2. account_id + area_id 复合索引（运维/合服使用）
        client.CreateIndex(
            defaults::kAccountInfoCollection,
            make_document(kvp("account_id", 1), kvp("area_id", 1)),
            /*unique=*/false,
            /*name=*/"idx_account_area",
            /*sparse=*/false,
            /*background=*/true);

        // 3. 充值排行索引（sparse 跳过 charge=0 的文档）
        client.CreateIndex(
            defaults::kAccountInfoCollection,
            make_document(kvp("charge", -1)),
            /*unique=*/false,
            /*name=*/"idx_charge",
            /*sparse=*/true,
            /*background=*/true);
    }

} // namespace mongo
