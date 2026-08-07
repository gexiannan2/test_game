// AOI 视野总控实现。

#include "ecs/systems/aoi_system.h"

#include <algorithm>
#include <unordered_set>

#include "ecs/entity/entity.h"
#include "ecs/systems/aoi_sector.h"
#include "ecs/systems/map_system.h"
#include "ecs/systems/world_system.h"

AoiSystem::AoiSystem() = default;
AoiSystem::~AoiSystem() = default;

void AoiSystem::Init(MapSystem* map) {
    map_ = map;
    // 重建 sector 前先卸掉旧 watcher，避免 receivers_ 孤儿
    for (auto it = watchers_.begin(); it != watchers_.end(); ++it) {
        if (AoiSector* load = Sector(it->second.detail_level)) {
            load->DetachWatcher(it->first, it->second.center, false);
        }
    }
    watchers_.clear();
    InitAoiSectors();
    // Init 后重绑回调，修复「Init 前 SetEntity*Callback」无效
    RebuildViewNotify();
}

void AoiSystem::InitAoiSectors() {
    aoi_sectors_.clear();
    if (!map_) return;
    for (size_t i = 0; i < kDetailLevelCount; ++i) {
        auto load = std::make_unique<AoiSector>();
        load->Init(map_, kDefaultCellSizes[i], static_cast<int>(i),
                   map_->GridStorage());
        aoi_sectors_.push_back(std::move(load));
    }
}

void AoiSystem::RebuildViewNotify() {
    if (!entity_enter_cb_ && !entity_leave_cb_ && !entity_update_cb_) {
        view_notify_ = {};
    } else {
        view_notify_ = [this](const AoiEvent& change) {
            switch (change.kind) {
                case AoiEventKind::kEnter:
                    if (entity_enter_cb_)
                        entity_enter_cb_(change.watcher_id, change.entity_ids);
                    break;
                case AoiEventKind::kLeave:
                    if (entity_leave_cb_)
                        entity_leave_cb_(change.watcher_id, change.entity_ids);
                    break;
                case AoiEventKind::kUpdate:
                    if (entity_update_cb_ && !change.entity_ids.empty())
                        entity_update_cb_(change.watcher_id, change.entity_ids.front());
                    break;
            }
        };
    }
    for (auto& load : aoi_sectors_) {
        if (load) load->SetViewNotify(view_notify_);
    }
    RebuildBroadcastNotify();
}

void AoiSystem::RebuildBroadcastNotify() {
    const bool has_per_watcher = entity_enter_cb_ || entity_leave_cb_ ||
                                 entity_update_cb_;
    if (!broadcast_cb_ && !has_per_watcher) {
        broadcast_notify_ = {};
    } else {
        broadcast_notify_ = [this](const AoiBroadcastEvent& ev) {
            if (broadcast_cb_) {
                broadcast_cb_(ev);
            }
            if (ev.watcher_ids.empty()) {
                return;
            }
            const std::vector<uint64_t> one = {ev.subject_id};
            switch (ev.kind) {
                case AoiEventKind::kEnter:
                    if (entity_enter_cb_) {
                        for (uint64_t wid : ev.watcher_ids) {
                            entity_enter_cb_(wid, one);
                        }
                    }
                    break;
                case AoiEventKind::kLeave:
                    if (entity_leave_cb_) {
                        for (uint64_t wid : ev.watcher_ids) {
                            entity_leave_cb_(wid, one);
                        }
                    }
                    break;
                case AoiEventKind::kUpdate:
                    if (entity_update_cb_) {
                        for (uint64_t wid : ev.watcher_ids) {
                            entity_update_cb_(wid, ev.subject_id);
                        }
                    }
                    break;
            }
        };
    }
    for (auto& load : aoi_sectors_) {
        if (load) load->SetBroadcastNotify(broadcast_notify_);
    }
}

void AoiSystem::SetEntityEnterCallback(EntityEnterCallback cb) {
    entity_enter_cb_ = std::move(cb);
    RebuildViewNotify();
}
void AoiSystem::SetEntityLeaveCallback(EntityLeaveCallback cb) {
    entity_leave_cb_ = std::move(cb);
    RebuildViewNotify();
}
void AoiSystem::SetEntityUpdateCallback(EntityUpdateCallback cb) {
    entity_update_cb_ = std::move(cb);
    RebuildViewNotify();
}
void AoiSystem::SetEntityBroadcastCallback(AoiBroadcastNotifyFn cb) {
    broadcast_cb_ = std::move(cb);
    RebuildBroadcastNotify();
}

void AoiSystem::OnEntityIntoMap(const EntityPtr& entity) {
    for (auto& load : aoi_sectors_) {
        if (load) load->OnSubjectEnterMap(entity);
    }
}

void AoiSystem::OnEntityLeaveMap(const EntityPtr& entity) {
    for (auto& load : aoi_sectors_) {
        if (load) load->OnSubjectLeaveMap(entity);
    }
}

void AoiSystem::OnEntityChangePos(const EntityPtr& entity,
                                   const Vector3D& old_pos,
                                   const Vector3D& new_pos) {
    if (!entity || !map_) return;
    if (!map_->IsInMap(old_pos) || !map_->IsInMap(new_pos)) return;
    for (auto& load : aoi_sectors_) {
        if (load) load->OnSubjectMoved(entity, old_pos, new_pos);
    }
}

void AoiSystem::NotifySelfAppear(const EntityPtr& entity) {
    // 仅在实体作为观察者的那一层 sector 补发自身 appear，
    // 避免跨层（near/far）重复推。
    if (!entity || !entity->IsInMap()) {
        return;
    }
    const uint64_t sid = entity->GetId();
    if (!HasWatcher(sid)) {
        return;
    }
    const int dl = WatcherDetailLevel(sid);
    AoiSector* load = Sector(dl);
    if (!load) {
        return;
    }
    AoiCell* cell = load->CellAtPosition(entity->GetPosition());
    if (!cell) {
        return;
    }
    cell->Monitor().NotifySelfAppear(sid);
}

int AoiSystem::WatcherDetailLevel(uint64_t viewer_id) const {
    auto it = watchers_.find(viewer_id);
    return (it != watchers_.end()) ? it->second.detail_level : 0;
}

bool AoiSystem::AddWatcher(const EntityPtr& viewer, const Vector3D& center,
                            int detail_level) {
    if (!viewer || !map_ || !map_->IsInMap(viewer->GetPosition())) return false;
    if (detail_level < 0 ||
        static_cast<size_t>(detail_level) >= kDetailLevelCount) {
        return false;
    }
    AoiSector* load = Sector(detail_level);
    if (!load) return false;
    const uint64_t viewer_id = viewer->GetId();
    if (HasWatcher(viewer_id)) {
        const WatcherState& state = watchers_.at(viewer_id);
        if (state.detail_level == detail_level && state.center == center) {
            return load->RefreshWatcher(viewer, center);
        }
        RemoveWatcher(viewer, false);
    }
    if (!load->AttachWatcher(viewer, center)) return false;
    watchers_[viewer_id] = WatcherState{detail_level, center};
    return true;
}

bool AoiSystem::MoveWatcher(const EntityPtr& viewer,
                             const Vector3D& old_center,
                             const Vector3D& new_center) {
    if (!viewer || !map_ || !map_->IsInMap(new_center)) return false;
    return MoveWatcher(viewer->GetId(), old_center, new_center);
}

bool AoiSystem::MoveWatcher(uint64_t viewer_id,
                             const Vector3D& old_center,
                             const Vector3D& new_center) {
    if (!world_) {
        return false;
    }
    auto it = watchers_.find(viewer_id);
    if (it == watchers_.end()) {
        return false;
    }
    AoiSector* load = Sector(it->second.detail_level);
    if (!load) return false;
    EntityPtr viewer = world_->FindEntity(viewer_id);
    if (!viewer) return false;
    // 以 watchers_ 记录的中心为准做差集（调用方 old_center 可能已过期）
    const Vector3D from = it->second.center;
    (void)old_center;
    if (!load->MoveWatcher(viewer, from, new_center)) return false;
    it->second.center = new_center;
    return true;
}

void AoiSystem::RemoveWatcher(const EntityPtr& viewer, bool notify) {
    if (!viewer) return;
    RemoveWatcher(viewer->GetId(), viewer->GetPosition(), notify);
}

void AoiSystem::RemoveWatcher(uint64_t viewer_id, const Vector3D& center,
                               bool notify) {
    auto it = watchers_.find(viewer_id);
    if (it == watchers_.end()) return;
    int detail_level = it->second.detail_level;
    Vector3D saved_center = it->second.center;
    watchers_.erase(it);
    if (AoiSector* load = Sector(detail_level)) {
        load->DetachWatcher(viewer_id, saved_center, notify);
    }
    (void)center;
}

bool AoiSystem::HasWatcher(uint64_t viewer_id) const {
    auto it = watchers_.find(viewer_id);
    if (it == watchers_.end()) return false;
    const AoiSector* load = Sector(it->second.detail_level);
    if (load && load->HasWatcher(viewer_id, it->second.center)) {
        return true;
    }
    // watchers_ 与 sector 失步：擦掉陈旧条目，避免跳过 MoveWatcher
    const_cast<AoiSystem*>(this)->watchers_.erase(it);
    return false;
}

void AoiSystem::MarkPropertyDirty(const EntityPtr& subject,
                                   bool sync_immediately) {
    if (!subject || !subject->IsInMap()) return;
    const uint64_t sid = subject->GetId();
    std::vector<AoiCell*> marked_cells;
    marked_cells.reserve(aoi_sectors_.size());
    for (auto& load : aoi_sectors_) {
        if (!load) continue;
        // 不 EnsureCell：避免在无观察者的远景层凭空建格、塞空 dirty_entities_
        AoiCell* cell = load->CellAtPosition(subject->GetPosition());
        if (!cell) continue;
        // 仅当本层已监控该 subject，且格内确有观察者时才入脏队列
        if (!cell->Monitor().IsMonitoring(sid)) continue;
        if (!cell->Monitor().HasAnyReceiver()) continue;
        cell->MarkSubjectDirty(subject);
        marked_cells.push_back(cell);
    }
    if (!sync_immediately) return;
    std::vector<EntityPtr> flushed;
    for (AoiCell* cell : marked_cells) {
        cell->FlushDirty(&flushed);
    }
    std::unordered_set<uint64_t> cleared;
    for (const EntityPtr& e : flushed) {
        if (!e) continue;
        if (cleared.insert(e->GetId()).second) {
            e->ClearPropertyTypes();
        }
    }
}

void AoiSystem::FlushDirty() {
    std::vector<EntityPtr> flushed;
    for (auto& load : aoi_sectors_) {
        if (load) load->FlushDirty(&flushed);
    }
    std::unordered_set<uint64_t> cleared;
    for (const EntityPtr& subject : flushed) {
        if (!subject) continue;
        if (cleared.insert(subject->GetId()).second) {
            subject->ClearPropertyTypes();
        }
    }
}

AoiSector* AoiSystem::Sector(int detail_level) {
    if (detail_level < 0 || static_cast<size_t>(detail_level) >= aoi_sectors_.size())
        return nullptr;
    return aoi_sectors_[static_cast<size_t>(detail_level)].get();
}

const AoiSector* AoiSystem::Sector(int detail_level) const {
    if (detail_level < 0 || static_cast<size_t>(detail_level) >= aoi_sectors_.size())
        return nullptr;
    return aoi_sectors_[static_cast<size_t>(detail_level)].get();
}

std::vector<uint64_t> AoiSystem::GetVisibleEntities(uint64_t watcher_id) const {
    std::vector<uint64_t> result;
    auto it = watchers_.find(watcher_id);
    if (it == watchers_.end()) return result;
    const AoiSector* load = Sector(it->second.detail_level);
    if (!load) return result;
    int32_t cx = 0, cy = 0, cz = 0;
    load->CellIndexAt(it->second.center, cx, cy, cz);
    int32_t min_x = 0, min_y = 0, min_z = 0, max_x = 0, max_y = 0, max_z = 0;
    CellToRange(cx, cy, cz, kNeighborhoodRadius, load->MaxCellX(),
                load->MaxCellY(), load->MaxCellZ(), min_x, min_y, min_z, max_x,
                max_y, max_z);
    std::unordered_set<uint64_t> seen;
    seen.reserve(64);
    for (int32_t z = min_z; z <= max_z; ++z) {
        for (int32_t y = min_y; y <= max_y; ++y) {
            for (int32_t x = min_x; x <= max_x; ++x) {
                const AoiCell* cell = load->CellAt(x, y, z);
                if (!cell) continue;
                std::vector<uint64_t> tmp;
                cell->Monitor().CollectMonitorNodeIds(tmp);
                for (uint64_t id : tmp) {
                    if (id == watcher_id) continue;
                    if (seen.insert(id).second) {
                        result.push_back(id);
                    }
                }
            }
        }
    }
    return result;
}
