#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>

#include "ecs/entity/entity.h"

// 玩家数据批量异步落地（统一 dirty 标记 + 定时扫描）：
//   - 业务点（移动/属性变更）调 PlayerDataComponent::SetDirty() 标脏（不立即 PostSave）
//   - 定时器周期性扫描 PlayerDataComponent::IsDirty() 的玩家批量落地（每批 <= N 个）
//   - 下线 FlushPlayer 立即落地；关服 FlushOnShutdown 全量排空
// 持久化状态仅在 GameServer 主线程（EventLoop）访问；完成回调由 worker 线程触发。

namespace mongo
{
    class PlayerMongoStorage;
} // namespace mongo

// 落地结果（callback 参数）
enum class PersistResult : int
{
    kPersisted = 0,    // 落地成功
    kPersistFailed,    // 落地失败（写库异常）
    kSkipped,          // 跳过（未启用 / 无 role / entity 已离线 / 快照组装失败）
    kQueueFull         // 入队失败（队列满 / 停服）
};

using PersistCallback = std::function<void(PersistResult)>;

class PlayerPersistSystem
{
    public:
        PlayerPersistSystem();
        ~PlayerPersistSystem();

        PlayerPersistSystem(const PlayerPersistSystem&) = delete;
        PlayerPersistSystem& operator=(const PlayerPersistSystem&) = delete;
        PlayerPersistSystem(PlayerPersistSystem&&) = delete;
        PlayerPersistSystem& operator=(PlayerPersistSystem&&) = delete;

        // 绑定异步存储；传 nullptr 表示禁用落地。
        void SetStorage(mongo::PlayerMongoStorage* storage);

        // 注入业务线程投递器（GameServer::RunInLoop 的包装），
        // worker 完成回调通过它回业务线程；必须在使用前设置（强制要求）。
        void SetPostToLoop(std::function<void(std::function<void()>)> post);

        bool IsEnabled() const noexcept;

        // 定时器周期调用：扫描 PlayerDataComponent::IsDirty() 的玩家批量落地（每批 <= maxBatch）。
        // 须在业务线程调用。
        void TickPersist(std::size_t maxBatch = 100);

        // 单玩家立即落地（下线/被踢）：从 dirty 取 callback，立即 PostOne，不等 tick。
        // 须在业务线程调用。
        void FlushPlayer(uint64_t roleId);

        // 关服排空：全量 TickPersist + WaitForIdle + Stop。须在业务线程调用。
        void FlushOnShutdown();

    private:
        uint64_t NextSequence(uint64_t roleId);
        // 落地单个 role_id：重查 entity -> SyncToData -> SerializeToDB -> PostSave(带 completion)。
        // 落地失败时 SetDirty 重试。须在业务线程调用。
        void PostOne(uint64_t roleId);

        mongo::PlayerMongoStorage* storage_ = nullptr;
        std::function<void(std::function<void()>)> post_to_loop_;
        // 关停标志：FlushOnShutdown 结束后置 true，后续 completion fire 不再访问实体/组件。
        std::atomic<bool> shutdown_{false};
        // role_id -> 进程内单调递增 sequence（用于乱序覆盖可观测）
        std::unordered_map<uint64_t, uint64_t> sequences_;
};
