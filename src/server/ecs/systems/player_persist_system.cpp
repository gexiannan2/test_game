#include "ecs/systems/player_persist_system.h"

#include "PlayerMongoStorage.h"

#include "MongoError.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common/thread_id.h"

#include "ecs/components/player_data_component.h"
#include "ecs/components/role_component.h"
#include "ecs/entity/entity.h"
#include "ecs/entity/player_entity.h"
#include "ecs/systems/player_entity_system.h"
#include "zrpc/base/logger.h"

// 玩家数据批量异步落地（统一 dirty 标记 + 定时扫描）：
//   - 业务点（移动/属性变更）调 PlayerDataComponent::SetDirty() 标脏（不立即 PostSave）
//   - 定时器周期性扫描 PlayerDataComponent::IsDirty() 的玩家批量落地（每批 <= N 个）
//   - 下线 FlushPlayer 立即落地；关服 FlushOnShutdown 全量排空
// 持久化状态仅在 GameServer 主线程（EventLoop）访问；完成回调由 worker 线程触发。

PlayerPersistSystem::PlayerPersistSystem() = default;

PlayerPersistSystem::~PlayerPersistSystem() = default;

void PlayerPersistSystem::SetStorage(mongo::PlayerMongoStorage* storage)
{
    storage_ = storage;
}

void PlayerPersistSystem::SetPostToLoop(std::function<void(std::function<void()>)> post)
{
    post_to_loop_ = std::move(post);
}

bool PlayerPersistSystem::IsEnabled() const noexcept
{
    return storage_ != nullptr;
}

uint64_t PlayerPersistSystem::NextSequence(uint64_t roleId)
{
    auto& seq = sequences_[roleId];
    return ++seq;
}

void PlayerPersistSystem::PostOne(uint64_t roleId)
{
    // 按 role_id 重查 entity（不持有 entity，避免阻止销毁）。
    auto entity = PlayerEntitySystem::Instance().FindByRoleId(roleId);
    if (!entity)
    {
        return;  // entity 已离线/销毁，跳过本次落地
    }

    auto* pdc = entity->GetComponent<PlayerDataComponent>();
    if (pdc == nullptr)
    {
        return;  // 无 PlayerDataComponent（未进图加载），跳过
    }

    // SyncToData 是 PlayerEntity 方法（非虚），需 cast。
    auto player = std::dynamic_pointer_cast<PlayerEntity>(entity);
    if (!player)
    {
        return;
    }
    // 运行时态 -> pdc->data（位置/属性写回，纯存储字段原样保留）。
    player->SyncToData();

    const uint64_t sequence = NextSequence(roleId);
    auto snapshot = entity->SerializeToDB(sequence);  // 返回 pdc->data
    if (!snapshot)
    {
        return;  // 快照组装失败，跳过
    }

    // 快照已取到当前态：先清脏。飞行中新变更会再 SetDirty；失败回调也会重标。
    // 若成功路径不 ClearDirty，TickPersist 会对同一玩家无限重投 PostSave。
    pdc->ClearDirty();

    // worker 写库完成后回业务线程；失败时 SetDirty 重试（最终一致，不回滚内存）。
    // completion 在 worker 线程执行，fire 经 post_to_loop 投回业务线程执行。
    
    
    //   - 旧代码 completion 在 worker 线程读 this->shutdown_，依赖 "FlushOnShutdown 同步排空
    //     worker 后 this 才析构"。但 GameServer 析构顺序为 player_persist_ 先于 player_storage_
    //     （声明逆序），若未走正常 Stop() 路径（异常/信号退出），player_persist_ 析构时
    //     worker 仍可能在执行 completion → 访问已析构的 this → UAF。
    //   - 改为在 PostOne 调用时（业务线程，this 必存活）快照 shutdown_ 值捕获进 completion，
    //     completion/fire 全程不访问 this，彻底解耦与析构顺序的依赖。
    //   - 配合 FlushOnShutdown 开头即置 shutdown_=true，关停期间投递的任务 completion 读到
    //     快照值 true，fire 不再 SetDirty 重试（TickPersist 已停，重试也无消费者）。
    const bool is_shutdown_now = shutdown_.load(std::memory_order_acquire);
    // 在 PostOne 调用时（业务 EventLoop 线程上下文，this 必存活）捕获当前内核线程 id，
    // 作为 fire 期望执行的线程 id。fire 实际执行时对比该 id：
    //   - 若 post_to_loop 正确切回 EventLoop 线程，fire 在此线程执行，两者相等；
    //   - 若 post_to_loop 同步在 worker 线程执行 fire（测试场景或异常路径），两者不等，
    //     日志会暴露这个情况，避免"看似回到业务线程实则没有"的隐蔽问题。
    // 用 syscall(SYS_gettid) 取内核小整数 tid，便于和 top -H / gdb / /proc/task 对应。
    const std::int64_t expected_eventloop_thread_id = e996::GetThreadId();
    auto completion =
        [roleId, sequence, post = post_to_loop_, is_shutdown_now, expected_eventloop_thread_id](
            bool success, std::int64_t, std::exception_ptr ep)
        {
            // 此处运行在 mongo worker 线程：记录写库失败的内核线程 id，便于排查跨线程传播。
            const std::int64_t mongo_worker_thread_id = e996::GetThreadId();
            auto fire = [roleId, sequence, success, ep, is_shutdown_now,
                         mongo_worker_thread_id, expected_eventloop_thread_id]()
            {
                // 此处应运行在业务 EventLoop 线程（经 post_to_loop 投回）。
                // expected_eventloop_thread_id 是 PostOne 调用时的线程（即 EventLoop 线程）；
                // 若 post_to_loop 正确切线程，两者应相等。
                const std::int64_t actual_fire_thread_id = e996::GetThreadId();
                const bool fire_in_expected_thread = (actual_fire_thread_id == expected_eventloop_thread_id);
                if (success)
                {
                    return;  // 落地成功（脏位已在入队前 ClearDirty）
                }
                try
                {
                    if (ep)
                    {
                        std::rethrow_exception(ep);
                    }
                }
                catch (const mongo::MongoError& me)
                {
                    // mongod 不可达/写失败等：打印操作名 + 错误码 + 线程 id + 原始消息，便于排查。
                    // 时间戳由 LOG_WARN 自动附加（Logger::Impl::FormatTime）。
                    // fire_in_expected_thread=false 表示 post_to_loop 未切回 EventLoop 线程
                    // （fire 被 worker 线程同步执行），需重点关注。
                    LOG_WARN << "player persist落地失败(将重试), role_id=" << roleId
                             << " sequence=" << sequence
                             << " op=" << me.operation()
                             << " code=" << me.code()
                             << " mongo_worker_thread_id=" << mongo_worker_thread_id
                             << " expected_eventloop_thread_id=" << expected_eventloop_thread_id
                             << " actual_fire_thread_id=" << actual_fire_thread_id
                             << " fire_in_expected_thread=" << (fire_in_expected_thread ? 1 : 0)
                             << " what=" << me.what();
                }
                catch (const std::exception& e)
                {
                    // 非 MongoError 的兜底（如 protobuf 序列化异常等）。
                    LOG_WARN << "player persist落地失败(将重试), role_id=" << roleId
                             << " sequence=" << sequence
                             << " code=" << -1
                             << " mongo_worker_thread_id=" << mongo_worker_thread_id
                             << " expected_eventloop_thread_id=" << expected_eventloop_thread_id
                             << " actual_fire_thread_id=" << actual_fire_thread_id
                             << " fire_in_expected_thread=" << (fire_in_expected_thread ? 1 : 0)
                             << " what=" << e.what();
                }
                // 关停后不再标脏重试（用快照值，不访问 this）。
                if (is_shutdown_now)
                {
                    return;
                }
                // 重新标脏，下个 tick 用最新位置重试。
                // PlayerEntitySystem 是单例，FindByRoleId 返回 shared_ptr（引用计数保活），安全。
                auto ent = PlayerEntitySystem::Instance().FindByRoleId(roleId);
                if (ent)
                {
                    auto* p = ent->GetComponent<PlayerDataComponent>();
                    if (p)
                    {
                        p->SetDirty();
                    }
                }
            };
            // 强制要求 post：fire 必须在业务线程执行（访问实体/组件）。
            assert(post && "SetPostToLoop must be called before PostSave");
            post(std::move(fire));
        };

    if (!storage_->PostSave(std::move(*snapshot), sequence, std::move(completion)))
    {
        LOG_WARN << "player persist: PostSave rejected (queue full or stopping), role_id=" << roleId;
        // 入队失败也重新标脏，下个 tick 重试（停服时 TickPersist 不再跑，无副作用）。
        if (!is_shutdown_now)
        {
            pdc->SetDirty();
        }
    }
}

void PlayerPersistSystem::TickPersist(std::size_t maxBatch)
{
    if (!IsEnabled() || maxBatch == 0)
    {
        return;
    }

    // 遍历所有玩家，收集 PlayerDataComponent::IsDirty() 的（<= maxBatch）。
    const auto all = PlayerEntitySystem::Instance().GetAllByRoleIdSnapshot();
    std::vector<uint64_t> batch;
    batch.reserve(std::min(maxBatch, all.size()));
    for (const auto& [roleId, entity] : all)
    {
        auto* pdc = entity->GetComponent<PlayerDataComponent>();
        if (pdc != nullptr && pdc->IsDirty())
        {
            batch.push_back(roleId);
            if (batch.size() >= maxBatch)
            {
                break;
            }
        }
    }

    for (uint64_t roleId : batch)
    {
        PostOne(roleId);
    }
}

void PlayerPersistSystem::FlushPlayer(uint64_t roleId)
{
    if (!IsEnabled() || roleId == 0)
    {
        return;
    }
    // 仅当该玩家有 dirty 的 PlayerDataComponent 时立即落地（不等 tick）。
    auto entity = PlayerEntitySystem::Instance().FindByRoleId(roleId);
    if (!entity)
    {
        return;
    }
    auto* pdc = entity->GetComponent<PlayerDataComponent>();
    if (pdc == nullptr || !pdc->IsDirty())
    {
        return;
    }
    PostOne(roleId);
}

void PlayerPersistSystem::FlushOnShutdown()
{
	shutdown_.store(true, std::memory_order_release);

    if (storage_ == nullptr)
    {
        // 未启用：已标记关停，直接返回。
        return;
    }

    // 全量落地（不分批）。
    TickPersist(std::numeric_limits<std::size_t>::max());

    storage_->RequestStop();
    constexpr int kShutdownWaitSec = 5;
    const bool idle = storage_->WaitForIdle(std::chrono::seconds(kShutdownWaitSec));
    const auto m = storage_->Metrics();
    if (!idle)
    {
        LOG_ERROR << "player persist shutdown: WaitForIdle timeout " << kShutdownWaitSec
                  << "s, posted=" << m.posted << " completed=" << m.completed
                  << " failed=" << m.failed << " rejected=" << m.rejected
                  << " queued=" << m.queued << " active=" << m.active;
    }
    else
    {
        LOG_INFO << "player persist shutdown: drained, posted=" << m.posted
                 << " completed=" << m.completed << " failed=" << m.failed
                 << " rejected=" << m.rejected;
    }

    storage_->Stop();
}
