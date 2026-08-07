// 测试用 AOI 回调绑定，避免每个测试重复写 Set*Callback。
//
// 注：进/离/更新地图的生命周期事件已迁移至 EventBus
// （EvtEnterMap/EvtLeaveMap/EvtMoveMap），原 MapSystem 回调工具已移除。
// 测试如需监听地图事件，直接 EventBus::Instance().Subscribe<EvtEnterMap>(...)。
#pragma once

#include <memory>
#include <vector>

#include "ecs/systems/aoi_system.h"
#include "ecs/systems/world_system.h"

namespace test {

// 把带 OnEntityEnter/Leave/Update 方法的对象绑到 AoiSystem。
template <typename Sync>
void BindAoiCallbacks(AoiSystem& aoi, const std::shared_ptr<Sync>& sync) {
    aoi.SetEntityEnterCallback(
        [sync](uint64_t viewer, const std::vector<uint64_t>& subjects) {
            sync->OnEntityEnter(viewer, subjects);
        });
    aoi.SetEntityLeaveCallback(
        [sync](uint64_t viewer, const std::vector<uint64_t>& subjects) {
            sync->OnEntityLeave(viewer, subjects);
        });
    aoi.SetEntityUpdateCallback([sync](uint64_t viewer, uint64_t subject) {
        sync->OnEntityUpdate(viewer, subject);
    });
}

template <typename Sync>
void BindAoiCallbacks(WorldSystem& world, const std::shared_ptr<Sync>& sync) {
    BindAoiCallbacks(world.Aoi(), sync);
}

// 清空 AOI 三类回调，释放对 Sync 的 shared_ptr 捕获。
inline void ClearAoiCallbacks(AoiSystem& aoi) {
    aoi.SetEntityEnterCallback({});
    aoi.SetEntityLeaveCallback({});
    aoi.SetEntityUpdateCallback({});
}

inline void ClearAoiCallbacks(WorldSystem& world) {
    ClearAoiCallbacks(world.Aoi());
}

}  // namespace test
