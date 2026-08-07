#pragma once

// 玩家存档业务门面：把 GameServer 与 mongo 直接耦合改为间接耦合。
// 三类操作分别对应：账号域 / 加载域 / 角色域，将来拆进程时
// 替换为对应 RPC 客户端即可，handler 与 GameServer 接口不动。
//
// 内存安全约定（避免 UAF / 悬空 / 空指针 / double free / 内存泄露）：
//   - 继承 enable_shared_from_this，回调用 weak_from_this() 捕获，
//     facade 析构后 lock() 失败丢弃，不访问悬空对象。
//   - GameServer 持 shared_ptr<PlayerArchiveService>，单一所有权，
//     RAII 自动释放，无裸指针持有方，无手动 delete。
//   - storage_ 可为 nullptr（mongo 未启用），所有方法入口判空，
//     立即安全降级：QueryRole 回 success=false、DeleteRoleArchive 回 success=false、
//     PostAccountInfoAfterLogin 直接 return（行为等价于原 GameServer 实现）。
//   - 须在业务线程访问（与其它业务对象一致，非线程安全）。

#include <cstdint>
#include <functional>
#include <memory>

#include "ecs/entity/entity.h"       // EntityPtr
#include "client_common.pb.h"       // entity_player_data

namespace zrpc
{
    class EventLoop;
} // namespace zrpc

namespace mongo
{
    class PlayerMongoStorage;
} // namespace mongo

class PlayerArchiveService
    : public std::enable_shared_from_this<PlayerArchiveService>
{
    public:
        PlayerArchiveService(::zrpc::EventLoop& loop,
                             mongo::PlayerMongoStorage* storage);
        ~PlayerArchiveService();

        PlayerArchiveService(const PlayerArchiveService&) = delete;
        PlayerArchiveService& operator=(const PlayerArchiveService&) = delete;
        PlayerArchiveService(PlayerArchiveService&&) = delete;
        PlayerArchiveService& operator=(PlayerArchiveService&&) = delete;

        // 1) 账号登录成功后异步幂等写入 account_info（账号域）。
        //    失败仅日志，不影响登录结果。storage_ 为空时直接 return。
        void PostAccountInfoAfterLogin(const EntityPtr& entity);

        // 2) 加载玩家存档：异步从 mongo 加载，回调在业务线程（加载域）。
        //    success=true 表示查到存档；data 为存档（无存档时默认构造）。
        //    storage_ 为空时立即在业务线程回调 success=false。
        void QueryRole(uint64_t roleId,
                       std::function<void(bool, ::entity_player_data)> cb);

        // 3) 异步删除玩家存档（角色域）。
        //    cb 在业务线程执行；默认空（删除结果通常不关心）。
        //    storage_ 为空时立即在业务线程回调 success=false。
        void DeleteRoleArchive(uint64_t roleId,
                               std::function<void(bool)> cb = {});

    private:
        // 非 owning 引用：EventLoop 由 GameServer 持有，facade 生命周期 <= GameServer。
        // self 活着 ⇒ GameServer 活着 ⇒ loop_ 活着。
        ::zrpc::EventLoop& loop_;

        // 非 owning 指针：mongo 未启用时为 nullptr，所有方法入口判空。
        mongo::PlayerMongoStorage* storage_;
};
