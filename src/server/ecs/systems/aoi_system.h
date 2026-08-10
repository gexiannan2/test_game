#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>
#include <functional>

#include "ecs/entity/entity.h"
#include "common/aoi_def.h"

class MapSystem;
class AoiSector;
class WorldSystem;

// AOI 视野总控：多 detail_level 的 AoiSector，统一维护 watchers_。
class AoiSystem {
 public:
    AoiSystem();
    ~AoiSystem();

    void Init(MapSystem* map);
    void BindMapWorld(MapSystem* map) { Init(map); }
    void BindWorld(WorldSystem* world) { world_ = world; }

    void SetEntityEnterCallback(EntityEnterCallback cb);
    void SetEntityLeaveCallback(EntityLeaveCallback cb);
    void SetEntityUpdateCallback(EntityUpdateCallback cb);
    void SetEntityBroadcastCallback(AoiBroadcastNotifyFn cb);

    // 实体进离图、移动（WorldSystem 调度入口）
    void OnEntityIntoMap(const EntityPtr& entity);
    void OnEntityLeaveMap(const EntityPtr& entity);
    void OnEntityChangePos(const EntityPtr& entity, const Vector3D& old_pos,
                           const Vector3D& new_pos);

    bool AddWatcher(const EntityPtr& viewer, const Vector3D& center,
                    int detail_level = 0);
    bool MoveWatcher(const EntityPtr& viewer, const Vector3D& old_center,
                     const Vector3D& new_center);
    void RemoveWatcher(const EntityPtr& viewer, bool notify = true);

    bool HasWatcher(uint64_t viewer_id) const;
    bool HasWatcher(const EntityPtr& viewer) const { return HasWatcher(viewer->GetId()); }
    void RemoveWatcher(uint64_t viewer_id, const Vector3D& center, bool notify);
    bool MoveWatcher(uint64_t viewer_id, const Vector3D& old_center,
                     const Vector3D& new_center);

    void MarkPropertyDirty(const EntityPtr& subject, bool sync_immediately = false);
    void FlushDirty();

    AoiSector* Sector(int detail_level);
    std::vector<uint64_t> GetVisibleEntities(uint64_t watcher_id) const;

 private:
    struct WatcherState {
        int detail_level = 0;
        Vector3D center;
    };

    void InitAoiSectors();
    void RebuildViewNotify();
    void RebuildBroadcastNotify();
    int WatcherDetailLevel(uint64_t viewer_id) const;

    MapSystem* map_ = nullptr;
    WorldSystem* world_ = nullptr;
    EntityEnterCallback entity_enter_cb_;
    EntityLeaveCallback entity_leave_cb_;
    EntityUpdateCallback entity_update_cb_;
    AoiBroadcastNotifyFn broadcast_cb_;
    ViewNotifyFn view_notify_;
    AoiBroadcastNotifyFn broadcast_notify_;
    std::vector<std::unique_ptr<AoiSector>> aoi_sectors_;
    std::unordered_map<uint64_t, WatcherState> watchers_;
};
