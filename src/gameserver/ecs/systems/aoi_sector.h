#pragma once

// AOI 视野分区：AoiSector → AoiCell → EntityMonitor（单格谁看谁）。

#include <cstdint>
#include <climits>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include "common/aoi_def.h"

class MapSystem;

// 无界地图：不钳制到 0；仅当 max_cell_* 非 INT32_MAX 时裁上界。
inline void CellToRange(int32_t center_x, int32_t center_y, int32_t center_z,
                        int32_t radius, int32_t max_cell_x, int32_t max_cell_y,
                        int32_t max_cell_z, int32_t& min_x, int32_t& min_y,
                        int32_t& min_z, int32_t& max_x, int32_t& max_y,
                        int32_t& max_z) {
    min_x = center_x - radius;
    min_y = center_y - radius;
    min_z = center_z - radius;
    max_x = center_x + radius;
    max_y = center_y + radius;
    max_z = center_z + radius;
    if (max_cell_x < INT32_MAX && max_x > max_cell_x) {
        max_x = max_cell_x;
    }
    if (max_cell_y < INT32_MAX && max_y > max_cell_y) {
        max_y = max_cell_y;
    }
    if (max_cell_z < INT32_MAX && max_z > max_cell_z) {
        max_z = max_cell_z;
    }
}

struct AoiCellKey {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;

    bool operator==(const AoiCellKey& o) const {
        return x == o.x && y == o.y && z == o.z;
    }
};

struct AoiCellKeyHash {
    size_t operator()(const AoiCellKey& k) const {
        return (static_cast<size_t>(k.x) * 73856093u) ^
               (static_cast<size_t>(k.y) * 19349663u) ^
               (static_cast<size_t>(k.z) * 83492791u);
    }
};

class AoiSector;

class EntityMonitor {
 public:
    void BindAoiSector(AoiSector* load) { aoi_sector_ = load; }

    void AddReceiver(const EntityPtr& watcher);
    bool RemoveReceiver(uint64_t watcher_id, bool notify);
    void RefreshReceiver(const EntityPtr& watcher);
    bool HasReceiver(uint64_t watcher_id) const;
    bool HasAnyReceiver() const { return !receivers_.empty(); }
    bool IsMonitoring(uint64_t subject_id) const {
        return monitor_nodes_.find(subject_id) != monitor_nodes_.end();
    }
    bool IsIdle() const;

    bool MonitorEntity(const EntityPtr& subject);
    bool UnmonitorEntity(uint64_t subject_id, bool notify);
    void SwitchMonitor(const EntityPtr& subject, EntityMonitor& dest);

    void SetEntityPropertyDirty(const EntityPtr& subject);
    void PushEntityUpdate(const EntityPtr& subject);
    // 不清 Entity 属性脏位；上层 FlushDirty 后统一 ClearPropertyTypes
    void FlushDirty(std::vector<EntityPtr>* flushed = nullptr);

    void CollectMonitorNodeIds(std::vector<uint64_t>& ids) const;
    void NotifyAppearToReceiver(uint64_t watcher_id,
                                const std::vector<uint64_t>& subject_ids);
    void NotifyAppearAllReceivers(const std::vector<uint64_t>& subject_ids);
    // 显式补发自身 appear（is_self=true）：仅进图时调用一次。
    void NotifySelfAppear(uint64_t subject_id);

 private:
    EntityPtr Lock(const std::weak_ptr<Entity>& weak) const;
    void EmitViewEvent(const AoiEvent& event) const;
    // 收集所有 watcher（排除 subject 自身）。
    void CollectReceiversExcept(uint64_t subject_id,
                                std::vector<uint64_t>& out_ids) const;
    // 发 per-subject 广播：序列化由桥接层做一次，遍历 watcher 发送。
    void BroadcastAppear(uint64_t subject_id,
                         const std::vector<uint64_t>& watcher_ids);
    void BroadcastDisappear(uint64_t subject_id,
                            const std::vector<uint64_t>& watcher_ids);
    void BroadcastUpdate(uint64_t subject_id,
                         const std::vector<uint64_t>& watcher_ids);
    void EmitBroadcast(const AoiBroadcastEvent& event) const;
    void NotifyDisappeared(uint64_t watcher_id,
                           const std::vector<uint64_t>& subject_ids);
    void NotifyUpdated(uint64_t watcher_id, uint64_t subject_id);

    AoiSector* aoi_sector_ = nullptr;
    std::unordered_map<uint64_t, std::weak_ptr<Entity>> receivers_;
    std::unordered_map<uint64_t, std::weak_ptr<Entity>> monitor_nodes_;
    std::unordered_map<uint64_t, std::weak_ptr<Entity>> dirty_entities_;
};

class AoiCell {
 public:
    AoiCell();
    explicit AoiCell(int32_t cell_x, int32_t cell_y, int32_t cell_z);

    void BindAoiSector(AoiSector* load);

    EntityMonitor& Monitor() { return monitor_; }
    const EntityMonitor& Monitor() const { return monitor_; }

    void SyncSubjectsFromMap();
    void AddViewer(const EntityPtr& watcher);
    void RefreshViewer(const EntityPtr& watcher);
    void RemoveViewer(uint64_t watcher_id, bool notify);

    void OnSubjectEnter(const EntityPtr& subject);
    void OnSubjectLeave(const EntityPtr& subject);
    void OnSubjectMoved(const EntityPtr& subject, AoiCell& from);

    void MarkSubjectDirty(const EntityPtr& subject);
    void FlushDirty(std::vector<EntityPtr>* flushed = nullptr);

    int32_t CellX() const { return cell_x_; }
    int32_t CellY() const { return cell_y_; }
    int32_t CellZ() const { return cell_z_; }
    bool IsIdle() const { return monitor_.IsIdle(); }

 private:
    AoiSector* load_ = nullptr;
    int32_t cell_x_ = 0;
    int32_t cell_y_ = 0;
    int32_t cell_z_ = 0;
    EntityMonitor monitor_;
};

class AoiSector {
 public:
    AoiSector();
    void Init(MapSystem* map, uint32_t cell_size_grids, int detail_level,
              MapGridStorage cell_storage);

    void ForEachSubjectInCell(int32_t cell_x, int32_t cell_y, int32_t cell_z,
                              const std::function<void(const EntityPtr&)>& fn) const;

    int32_t MaxCellX() const { return max_cell_x_; }
    int32_t MaxCellY() const { return max_cell_y_; }
    int32_t MaxCellZ() const { return max_cell_z_; }
    uint32_t CellSizeGrids() const { return cell_size_grids_; }
    int DetailLevel() const { return detail_level_; }
    MapGridStorage CellStorage() const { return cell_storage_; }

    AoiCell* CellAt(int32_t x, int32_t y, int32_t z);
    const AoiCell* CellAt(int32_t x, int32_t y, int32_t z) const;
    AoiCell* CellAtPosition(const Vector3D& pos);
    AoiCell* EnsureCellAtPosition(const Vector3D& pos);
    void CellIndexAt(const Vector3D& pos, int32_t& cell_x, int32_t& cell_y,
                     int32_t& cell_z) const;

    bool HasWatcher(uint64_t watcher_id, const Vector3D& watch_center) const;
    bool AttachWatcher(const EntityPtr& watcher, const Vector3D& watch_center);
    bool RefreshWatcher(const EntityPtr& watcher, const Vector3D& watch_center);
    void DetachWatcher(uint64_t watcher_id, const Vector3D& watch_center, bool notify);
    bool MoveWatcher(const EntityPtr& watcher, const Vector3D& old_center,
                     const Vector3D& new_center);

    void OnSubjectEnterMap(const EntityPtr& subject);
    void OnSubjectLeaveMap(const EntityPtr& subject);
    void OnSubjectMoved(const EntityPtr& subject, const Vector3D& old_pos,
                        const Vector3D& new_pos);

    void FlushDirty(std::vector<EntityPtr>* flushed = nullptr);
    void SetViewNotify(ViewNotifyFn notify);
    void NotifyView(const AoiEvent& event) const;
    void SetBroadcastNotify(AoiBroadcastNotifyFn notify);
    void NotifyBroadcast(const AoiBroadcastEvent& event) const;

    size_t AllocatedCellCount() const;

 private:
    bool IsCellIndexInBounds(int32_t x, int32_t y, int32_t z) const;
    size_t FlatCellIndex(int32_t x, int32_t y, int32_t z) const;
    AoiCell* EnsureCell(int32_t x, int32_t y, int32_t z);
    void ReleaseHashCellIfEmpty(int32_t x, int32_t y, int32_t z);
    void ReleaseHashCellIfEmpty(AoiCell& cell);
    void ReleaseIdleHashCells();
    void ForNeighborhood(int32_t center_x, int32_t center_y, int32_t center_z,
                         const std::function<void(AoiCell&)>& fn,
                         bool ensure_cells = true);
    void ForEachCell(const std::function<void(AoiCell&)>& fn);
    // fn 返回 false 时停止遍历（LeaveMap 幽灵清扫 early-exit）
    void ForEachCellUntil(const std::function<bool(AoiCell&)>& fn);

    MapSystem* map_ = nullptr;
    MapGridStorage cell_storage_ = MapGridStorage::kArray;
    uint32_t cell_size_grids_;
    int32_t max_cell_x_;
    int32_t max_cell_y_;
    int32_t max_cell_z_;
    uint32_t cells_per_x_;
    uint32_t cells_per_y_;
    uint32_t cells_per_z_;
    int detail_level_;
    std::vector<std::unique_ptr<AoiCell>> array_cells_;
    std::unordered_map<AoiCellKey, std::unique_ptr<AoiCell>, AoiCellKeyHash>
        hash_cells_;
    ViewNotifyFn view_notify_;
    AoiBroadcastNotifyFn broadcast_notify_;
};
