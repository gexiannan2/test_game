// 测试用 AOI/Map 回调绑定，避免每个测试重复写 Set*Callback。
#pragma once

#include <memory>
#include <vector>

#include "ecs/systems/aoi_system.h"
#include "ecs/systems/map_system.h"
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

// 清空地图进离图/移动/穿格回调。
inline void ClearMapCallbacks(MapSystem& map) {
    map.SetEnterMapCallback({});
    map.SetLeaveMapCallback({});
    map.SetMoveCallback({});
    map.SetCrossGridCallback({});
}

inline void ClearMapCallbacks(WorldSystem& world) {
    ClearMapCallbacks(world.Map());
}

// 把带 OnEnterMap/OnLeaveMap/OnMove/OnCrossGrid 的监听对象绑到 MapSystem。
template <typename Listener>
void BindMapCallbacks(MapSystem& map, const std::shared_ptr<Listener>& listener) {
    map.SetEnterMapCallback(
        [listener](const EntityPtr& e) { listener->OnEnterMap(e); });
    map.SetLeaveMapCallback(
        [listener](const EntityPtr& e) { listener->OnLeaveMap(e); });
    map.SetMoveCallback([listener](const EntityPtr& e, const Vector3D& old_pos,
                                   const Vector3D& new_pos) {
        listener->OnMove(e, old_pos, new_pos);
    });
    map.SetCrossGridCallback(
        [listener](const EntityPtr& e, uint32_t ogx, uint32_t ogy, uint32_t ogz,
                   uint32_t ngx, uint32_t ngy, uint32_t ngz) {
            listener->OnCrossGrid(e, ogx, ogy, ogz, ngx, ngy, ngz);
        });
}

template <typename Listener>
void BindMapCallbacks(WorldSystem& world,
                      const std::shared_ptr<Listener>& listener) {
    BindMapCallbacks(world.Map(), listener);
}

}  // namespace test
