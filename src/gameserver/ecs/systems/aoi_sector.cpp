// AOI 视野分区与 EntityMonitor 实现。

#include "ecs/systems/aoi_sector.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>

#include "ecs/entity/entity.h"
#include "ecs/systems/map_system.h"
#include "zrpc/base/logger.h"

void EntityMonitor::EmitViewEvent(const AoiEvent& event) const {
    if (aoi_sector_) {
        aoi_sector_->NotifyView(event);
    }
}

void AoiSector::SetViewNotify(ViewNotifyFn notify) {
    view_notify_ = std::move(notify);
}

void AoiSector::NotifyView(const AoiEvent& event) const {
    if (view_notify_) {
        view_notify_(event);
    }
}

void AoiSector::SetBroadcastNotify(AoiBroadcastNotifyFn notify) {
    broadcast_notify_ = std::move(notify);
}

void AoiSector::NotifyBroadcast(const AoiBroadcastEvent& event) const {
    if (broadcast_notify_) {
        broadcast_notify_(event);
    }
}

EntityPtr EntityMonitor::Lock(const std::weak_ptr<Entity>& weak) const {
    return weak.lock();
}

// watcher_ids 转短字符串, 用于日志
namespace {
std::string WatcherIdsStr(const std::vector<uint64_t>& ids) {
    if (ids.empty()) return "[]";
    if (ids.size() <= 5) {
        std::ostringstream oss;
        oss << "[";
        for (size_t i = 0; i < ids.size(); ++i) {
            if (i) oss << ",";
            oss << ids[i];
        }
        oss << "]";
        return oss.str();
    }
    return "[" + std::to_string(ids[0]) + "..." +
           std::to_string(ids.back()) + "](x" + std::to_string(ids.size()) + ")";
}
}  // namespace

void EntityMonitor::EmitBroadcast(const AoiBroadcastEvent& event) const {
    if (aoi_sector_) {
        aoi_sector_->NotifyBroadcast(event);
    }
}

// 收集所有 receiver id（排除 subject 自身与已失效 weak_ptr）。
void EntityMonitor::CollectReceiversExcept(uint64_t subject_id,
                                           std::vector<uint64_t>& out_ids) const {
    out_ids.clear();
    out_ids.reserve(receivers_.size());
    for (const auto& kv : receivers_) {
        if (kv.first == subject_id) {
            continue;
        }
        if (!Lock(kv.second)) {
            continue;  // 过期条目跳过；清理由 IsIdle/Release 路径处理
        }
        out_ids.push_back(kv.first);
    }
}

void EntityMonitor::BroadcastAppear(uint64_t subject_id,
                                     const std::vector<uint64_t>& watcher_ids) {
    if (watcher_ids.empty()) {
        return;
    }
    LOG_INFO << "[AOI] APPEAR  subject=" << subject_id
             << "  watchers" << WatcherIdsStr(watcher_ids);
    AoiBroadcastEvent ev;
    ev.kind = AoiEventKind::kEnter;
    ev.subject_id = subject_id;
    ev.watcher_ids = watcher_ids;
    EmitBroadcast(ev);
}

void EntityMonitor::BroadcastDisappear(uint64_t subject_id,
                                        const std::vector<uint64_t>& watcher_ids) {
    if (watcher_ids.empty()) {
        return;
    }
    LOG_INFO << "[AOI] DISAPPEAR  subject=" << subject_id
             << "  watchers" << WatcherIdsStr(watcher_ids);
    AoiBroadcastEvent ev;
    ev.kind = AoiEventKind::kLeave;
    ev.subject_id = subject_id;
    ev.watcher_ids = watcher_ids;
    EmitBroadcast(ev);
}

void EntityMonitor::BroadcastUpdate(uint64_t subject_id,
                                    const std::vector<uint64_t>& watcher_ids) {
    if (watcher_ids.empty()) {
        return;
    }
    LOG_INFO << "[AOI] UPDATE  subject=" << subject_id
             << "  watchers" << WatcherIdsStr(watcher_ids);
    AoiBroadcastEvent ev;
    ev.kind = AoiEventKind::kUpdate;
    ev.subject_id = subject_id;
    ev.watcher_ids = watcher_ids;
    EmitBroadcast(ev);
}

void EntityMonitor::NotifyAppearToReceiver(
    uint64_t watcher_id, const std::vector<uint64_t>& subject_ids) {
    // 单 watcher 新进入看到一批 subject：对每个 subject 发 per-subject 广播，
    // watcher_ids=[watcher_id]。序列化由桥接层做，每个 subject 仍只序列化一次。
    if (subject_ids.empty()) {
        return;
    }
    const std::vector<uint64_t> one = {watcher_id};
    for (uint64_t sid : subject_ids) {
        // 跳过自身：自身 appear 由 EnterMap → AoiSystem::NotifySelfAppear 显式补发，
        // 避免 AddReceiver（移动跨格 RemoveWatcher+AddWatcher）时重复推自身 appear。
        if (sid == watcher_id) {
            continue;
        }
        BroadcastAppear(sid, one);
    }
}

void EntityMonitor::NotifySelfAppear(uint64_t subject_id) {
    // 显式补发自身 appear（is_self=true）：仅进图时由 AoiSystem 调用一次。
    if (monitor_nodes_.find(subject_id) == monitor_nodes_.end()) {
        return;
    }
    const std::vector<uint64_t> one = {subject_id};
    BroadcastAppear(subject_id, one);
}

void EntityMonitor::NotifyAppearAllReceivers(
    const std::vector<uint64_t>& subject_ids) {
    // 一个 subject 进入格子，通知该格所有 watcher：
    // 对每个 subject 收集所有 watcher（排除自身）发一条广播，序列化一次遍历发送。
    if (subject_ids.empty()) {
        return;
    }
    std::vector<uint64_t> watcher_ids;
    for (uint64_t sid : subject_ids) {
        CollectReceiversExcept(sid, watcher_ids);
        BroadcastAppear(sid, watcher_ids);
    }
}

void EntityMonitor::NotifyDisappeared(uint64_t watcher_id,
                                      const std::vector<uint64_t>& subject_ids) {
    // 单 watcher 离开看到一批 subject 消失：对每个 subject 发广播，watcher_ids=[watcher_id]。
    if (subject_ids.empty()) {
        return;
    }
    const std::vector<uint64_t> one = {watcher_id};
    for (uint64_t sid : subject_ids) {
        if (sid != watcher_id) {
            BroadcastDisappear(sid, one);
        }
    }
}

void EntityMonitor::NotifyUpdated(uint64_t watcher_id, uint64_t subject_id) {
    // 单 watcher 维度的增量更新（SwitchMonitor 差集路径）：发广播，watcher_ids=[watcher_id]。
    if (watcher_id == subject_id) {
        return;
    }
    const std::vector<uint64_t> one = {watcher_id};
    BroadcastUpdate(subject_id, one);
}

void EntityMonitor::AddReceiver(const EntityPtr& watcher) {
    if (!watcher) {
        return;
    }
    const uint64_t wid = watcher->GetId();
    const size_t subj = monitor_nodes_.size();
    if (subj > 0) {
        LOG_INFO << "[AOI] AddReceiver  watcher=" << wid
                 << "  subjects=" << subj;
    }
    receivers_[wid] = watcher;

    std::vector<uint64_t> ids;
    CollectMonitorNodeIds(ids);
    if (!ids.empty()) {
        NotifyAppearToReceiver(wid, ids);
    }
}

void EntityMonitor::RefreshReceiver(const EntityPtr& watcher) {
    if (!watcher) {
        return;
    }
    const uint64_t wid = watcher->GetId();
    const bool already = receivers_.find(wid) != receivers_.end();
    receivers_[wid] = watcher;
    // 已在观察：只刷新 weak_ptr，避免重复推全量 appear（客户端幽灵叠层）
    if (already) {
        return;
    }
    std::vector<uint64_t> ids;
    CollectMonitorNodeIds(ids);
    if (!ids.empty()) {
        LOG_INFO << "[AOI] RefreshReceiver  watcher=" << wid
                 << "  notifySubjects=" << ids.size();
        NotifyAppearToReceiver(wid, ids);
    }
}

bool EntityMonitor::RemoveReceiver(uint64_t watcher_id, bool notify) {
    auto it = receivers_.find(watcher_id);
    if (it == receivers_.end()) {
        return false;
    }
    if (notify) {
        const size_t subj = monitor_nodes_.size();
        if (subj > 0) {
            LOG_INFO << "[AOI] RemoveReceiver  watcher=" << watcher_id
                     << "  subjects=" << subj;
        }
        std::vector<uint64_t> ids;
        CollectMonitorNodeIds(ids);
        if (!ids.empty()) {
            NotifyDisappeared(watcher_id, ids);
        }
    }
    receivers_.erase(it);
    return true;
}

bool EntityMonitor::HasReceiver(uint64_t watcher_id) const {
    return receivers_.find(watcher_id) != receivers_.end();
}

bool EntityMonitor::IsIdle() const {
    return receivers_.empty() && monitor_nodes_.empty() &&
           dirty_entities_.empty();
}

bool EntityMonitor::MonitorEntity(const EntityPtr& subject) {
    if (!subject) {
        return false;
    }
    const uint64_t sid = subject->GetId();
    if (monitor_nodes_.find(sid) != monitor_nodes_.end()) {
        return false;
    }
    LOG_INFO << "[AOI] MonitorEntity  subject=" << sid
             << "  receivers=" << receivers_.size();
    monitor_nodes_[sid] = subject;
    std::vector<uint64_t> ids = {sid};
    NotifyAppearAllReceivers(ids);
    return true;
}

bool EntityMonitor::UnmonitorEntity(uint64_t subject_id, bool notify) {
    auto it = monitor_nodes_.find(subject_id);
    if (it == monitor_nodes_.end()) {
        return false;
    }
    LOG_INFO << "[AOI] UnmonitorEntity  subject=" << subject_id
             << "  notify=" << notify
             << "  receivers=" << receivers_.size();
    dirty_entities_.erase(subject_id);
    monitor_nodes_.erase(it);
    if (notify) {
        // subject 离开：收齐所有 watcher 一次性广播，序列化 disappear 一次遍历发送。
        std::vector<uint64_t> watcher_ids;
        CollectReceiversExcept(subject_id, watcher_ids);
        BroadcastDisappear(subject_id, watcher_ids);
    }
    return true;
}

void EntityMonitor::SwitchMonitor(const EntityPtr& subject,
                                  EntityMonitor& dest) {
    if (!subject) {
        return;
    }
    const uint64_t subject_id = subject->GetId();

    // footprint 重叠：补差集 appear/disappear
    if (dest.monitor_nodes_.find(subject_id) != dest.monitor_nodes_.end()) {
        const bool was_dirty =
            dirty_entities_.find(subject_id) != dirty_entities_.end();
        monitor_nodes_.erase(subject_id);
        dirty_entities_.erase(subject_id);
        const std::vector<uint64_t> one = {subject_id};
        for (const auto& kv : receivers_) {
            if (!dest.HasReceiver(kv.first)) {
                NotifyDisappeared(kv.first, one);
            }
        }
        for (const auto& kv : dest.receivers_) {
            if (!HasReceiver(kv.first)) {
                dest.NotifyAppearToReceiver(kv.first, one);
            }
        }
        if (was_dirty) {
            dest.SetEntityPropertyDirty(subject);
        }
        return;
    }

    dest.monitor_nodes_[subject_id] = subject;

    const bool is_dirty =
        dirty_entities_.find(subject_id) != dirty_entities_.end() &&
        monitor_nodes_.find(subject_id) != monitor_nodes_.end();

    monitor_nodes_.erase(subject_id);
    dirty_entities_.erase(subject_id);

    std::unordered_map<uint64_t, EntityPtr> remove_receivers;
    std::unordered_map<uint64_t, EntityPtr> add_receivers;
    std::unordered_map<uint64_t, EntityPtr> keep_receivers;

    for (const auto& kv : receivers_) {
        if (!dest.HasReceiver(kv.first)) {
            if (EntityPtr w = Lock(kv.second)) {
                remove_receivers[kv.first] = w;
            }
        }
    }
    for (const auto& kv : dest.receivers_) {
        if (!HasReceiver(kv.first)) {
            if (EntityPtr w = dest.Lock(kv.second)) {
                add_receivers[kv.first] = w;
            }
        }
    }

    if (is_dirty) {
        for (const auto& kv : dest.receivers_) {
            if (add_receivers.find(kv.first) == add_receivers.end()) {
                if (EntityPtr w = dest.Lock(kv.second)) {
                    keep_receivers[kv.first] = w;
                }
            }
        }
        for (const auto& kv : keep_receivers) {
            if (kv.first != subject_id) {
                dest.NotifyUpdated(kv.first, subject_id);
            }
        }
        // 脏同步防双推：keep 已即时 kUpdate，不再入脏队列
        subject->ClearPropertyTypes();
    }

    const std::vector<uint64_t> one = {subject_id};
    for (const auto& kv : add_receivers) {
        dest.NotifyAppearToReceiver(kv.first, one);
    }
    for (const auto& kv : remove_receivers) {
        NotifyDisappeared(kv.first, one);
    }
}

void EntityMonitor::SetEntityPropertyDirty(const EntityPtr& subject) {
    if (subject) {
        dirty_entities_[subject->GetId()] = subject;
    }
}

void EntityMonitor::PushEntityUpdate(const EntityPtr& subject) {
    if (!subject) {
        return;
    }
    const uint64_t sid = subject->GetId();
    if (monitor_nodes_.find(sid) == monitor_nodes_.end()) {
        return;
    }
    // 与 NotifyAppearToReceiver 一致：不向自身推 kUpdate
    // subject 脏更新：收齐所有 watcher 一次性广播，序列化 dirty 一次遍历发送。
    std::vector<uint64_t> watcher_ids;
    CollectReceiversExcept(sid, watcher_ids);
    BroadcastUpdate(sid, watcher_ids);
}

void EntityMonitor::FlushDirty(std::vector<EntityPtr>* flushed) {
    if (dirty_entities_.empty()) {
        return;
    }
    for (const auto& kv : dirty_entities_) {
        if (EntityPtr subject = Lock(kv.second)) {
            if (!subject->PropertyTypes().empty()) {
                PushEntityUpdate(subject);
                if (flushed != nullptr) {
                    // 多层 sector 时由 AoiSystem 在所有层推送完后再统一 ClearPropertyTypes，
                    // 此处提前清会导致后续 detail_level 的远景观察者丢更新。
                    flushed->push_back(subject);
                } else {
                    subject->ClearPropertyTypes();
                }
            }
        }
    }
    dirty_entities_.clear();
}

void EntityMonitor::CollectMonitorNodeIds(std::vector<uint64_t>& ids) const {
    ids.clear();
    ids.reserve(monitor_nodes_.size());
    for (const auto& kv : monitor_nodes_) {
        ids.push_back(kv.first);
    }
}

AoiCell::AoiCell() = default;

AoiCell::AoiCell(int32_t cell_x, int32_t cell_y, int32_t cell_z)
    : cell_x_(cell_x), cell_y_(cell_y), cell_z_(cell_z) {}

void AoiCell::BindAoiSector(AoiSector* load) {
    load_ = load;
    monitor_.BindAoiSector(load);
}

void AoiCell::SyncSubjectsFromMap() {
    if (!load_) {
        return;
    }
    load_->ForEachSubjectInCell(
        cell_x_, cell_y_, cell_z_, [this](const EntityPtr& entity) {
            if (!entity) {
                return;
            }
            int32_t home_x = 0;
            int32_t home_y = 0;
            int32_t home_z = 0;
            load_->CellIndexAt(entity->GetPosition(), home_x, home_y, home_z);
            if (home_x != cell_x_ || home_y != cell_y_ || home_z != cell_z_) {
                return;
            }
            monitor_.MonitorEntity(entity);
        });
}

void AoiCell::AddViewer(const EntityPtr& watcher) {
    SyncSubjectsFromMap();
    monitor_.AddReceiver(watcher);
}

void AoiCell::RefreshViewer(const EntityPtr& watcher) {
    SyncSubjectsFromMap();
    monitor_.RefreshReceiver(watcher);
}

void AoiCell::RemoveViewer(uint64_t watcher_id, bool notify) {
    monitor_.RemoveReceiver(watcher_id, notify);
}

void AoiCell::OnSubjectEnter(const EntityPtr& subject) {
    monitor_.MonitorEntity(subject);
}

void AoiCell::OnSubjectLeave(const EntityPtr& subject) {
    if (subject) {
        monitor_.UnmonitorEntity(subject->GetId(), true);
    }
}

void AoiCell::OnSubjectMoved(const EntityPtr& subject, AoiCell& from) {
    if (!subject) {
        return;
    }
    from.monitor_.SwitchMonitor(subject, monitor_);
}

void AoiCell::MarkSubjectDirty(const EntityPtr& subject) {
    monitor_.SetEntityPropertyDirty(subject);
}

void AoiCell::FlushDirty(std::vector<EntityPtr>* flushed) {
    monitor_.FlushDirty(flushed);
}

AoiSector::AoiSector()
    : cell_size_grids_(kAoiCellWorldSize),
      max_cell_x_(0),
      max_cell_y_(0),
      max_cell_z_(0),
      cells_per_x_(0),
      cells_per_y_(0),
      cells_per_z_(0),
      detail_level_(0) {}

bool AoiSector::IsCellIndexInBounds(int32_t /*x*/, int32_t /*y*/,
                                     int32_t /*z*/) const {
    return cell_storage_ == MapGridStorage::kHash;
}

size_t AoiSector::FlatCellIndex(int32_t x, int32_t y, int32_t z) const {
    return static_cast<size_t>(z) * cells_per_x_ * cells_per_y_ +
           static_cast<size_t>(y) * cells_per_x_ + static_cast<size_t>(x);
}

void AoiSector::Init(MapSystem* map, uint32_t cell_size_grids, int detail_level,
                     MapGridStorage /*cell_storage*/) {
    map_ = map;
    cell_size_grids_ = cell_size_grids > 0 ? cell_size_grids : 1;
    detail_level_ = detail_level;
    cell_storage_ = MapGridStorage::kHash;

    cells_per_x_ = 0;
    cells_per_y_ = 0;
    cells_per_z_ = 0;
    max_cell_x_ = INT32_MAX;
    max_cell_y_ = INT32_MAX;
    max_cell_z_ = INT32_MAX;

    array_cells_.clear();
    hash_cells_.clear();
}

AoiCell* AoiSector::EnsureCell(int32_t x, int32_t y, int32_t z) {
    if (!IsCellIndexInBounds(x, y, z)) {
        return nullptr;
    }
    if (cell_storage_ == MapGridStorage::kArray) {
        return array_cells_[FlatCellIndex(x, y, z)].get();
    }
    AoiCellKey key{x, y, z};
    auto it = hash_cells_.find(key);
    if (it != hash_cells_.end()) {
        return it->second.get();
    }
    auto cell = std::make_unique<AoiCell>(x, y, z);
    cell->BindAoiSector(this);
    AoiCell* raw = cell.get();
    hash_cells_.emplace(key, std::move(cell));
    return raw;
}

void AoiSector::ReleaseHashCellIfEmpty(int32_t x, int32_t y, int32_t z) {
    if (cell_storage_ != MapGridStorage::kHash) {
        return;
    }
    const auto it = hash_cells_.find(AoiCellKey{x, y, z});
    if (it == hash_cells_.end() || !it->second) {
        return;
    }
    if (it->second->IsIdle()) {
        hash_cells_.erase(it);
    }
}

void AoiSector::ReleaseHashCellIfEmpty(AoiCell& cell) {
    ReleaseHashCellIfEmpty(cell.CellX(), cell.CellY(), cell.CellZ());
}

void AoiSector::ReleaseIdleHashCells() {
    if (cell_storage_ != MapGridStorage::kHash) {
        return;
    }
    for (auto it = hash_cells_.begin(); it != hash_cells_.end();) {
        if (!it->second || it->second->IsIdle()) {
            it = hash_cells_.erase(it);
        } else {
            ++it;
        }
    }
}

size_t AoiSector::AllocatedCellCount() const {
    if (cell_storage_ == MapGridStorage::kArray) {
        return array_cells_.size();
    }
    return hash_cells_.size();
}

void AoiSector::ForEachSubjectInCell(
    int32_t cell_x, int32_t cell_y, int32_t cell_z,
    const std::function<void(const EntityPtr&)>& fn) const {
    if (!fn || !map_ || !IsCellIndexInBounds(cell_x, cell_y, cell_z)) {
        return;
    }
    int32_t gx0 = 0;
    int32_t gy0 = 0;
    int32_t gz0 = 0;
    int32_t gx1 = 0;
    int32_t gy1 = 0;
    int32_t gz1 = 0;
    AoiCellToMapGridBox(cell_x, cell_y, cell_z, gx0, gy0, gz0, gx1, gy1, gz1,
                        cell_size_grids_, kGridSize);
    map_->CollectEntitiesInGridBox(gx0, gy0, gz0, gx1, gy1, gz1, fn);
}

const AoiCell* AoiSector::CellAt(int32_t x, int32_t y, int32_t z) const {
    if (!IsCellIndexInBounds(x, y, z)) {
        return nullptr;
    }
    if (cell_storage_ == MapGridStorage::kArray) {
        const size_t idx = FlatCellIndex(x, y, z);
        return idx < array_cells_.size() ? array_cells_[idx].get() : nullptr;
    }
    const auto it = hash_cells_.find(AoiCellKey{x, y, z});
    return it != hash_cells_.end() ? it->second.get() : nullptr;
}

AoiCell* AoiSector::CellAt(int32_t x, int32_t y, int32_t z) {
    return const_cast<AoiCell*>(
        static_cast<const AoiSector*>(this)->CellAt(x, y, z));
}

void AoiSector::CellIndexAt(const Vector3D& pos, int32_t& cell_x,
                              int32_t& cell_y, int32_t& cell_z) const {
    cell_x = WorldToAoiCellIndex(pos.GetX(), cell_size_grids_);
    cell_y = WorldToAoiCellIndex(pos.GetY(), cell_size_grids_);
    cell_z = WorldToAoiCellIndex(pos.GetZ(), cell_size_grids_);
}

AoiCell* AoiSector::CellAtPosition(const Vector3D& pos) {
    int32_t cx = 0;
    int32_t cy = 0;
    int32_t cz = 0;
    CellIndexAt(pos, cx, cy, cz);
    return CellAt(cx, cy, cz);
}

AoiCell* AoiSector::EnsureCellAtPosition(const Vector3D& pos) {
    int32_t cx = 0;
    int32_t cy = 0;
    int32_t cz = 0;
    CellIndexAt(pos, cx, cy, cz);
    return EnsureCell(cx, cy, cz);
}

void AoiSector::ForEachCell(const std::function<void(AoiCell&)>& fn) {
    if (!fn) {
        return;
    }
    ForEachCellUntil([&fn](AoiCell& cell) {
        fn(cell);
        return true;
    });
}

void AoiSector::ForEachCellUntil(const std::function<bool(AoiCell&)>& fn) {
    if (!fn) {
        return;
    }
    if (cell_storage_ == MapGridStorage::kArray) {
        for (auto& cell : array_cells_) {
            if (cell && !fn(*cell)) {
                return;
            }
        }
        return;
    }
    for (auto& kv : hash_cells_) {
        if (kv.second && !fn(*kv.second)) {
            return;
        }
    }
}

void AoiSector::ForNeighborhood(int32_t center_x, int32_t center_y,
                                  int32_t center_z,
                                  const std::function<void(AoiCell&)>& fn,
                                  bool ensure_cells) {
    int32_t min_x = 0;
    int32_t min_y = 0;
    int32_t min_z = 0;
    int32_t max_x = 0;
    int32_t max_y = 0;
    int32_t max_z = 0;
    CellToRange(center_x, center_y, center_z, kNeighborhoodRadius, max_cell_x_,
                max_cell_y_, max_cell_z_, min_x, min_y, min_z, max_x, max_y,
                max_z);
    for (int32_t z = min_z; z <= max_z; ++z) {
        for (int32_t y = min_y; y <= max_y; ++y) {
            for (int32_t x = min_x; x <= max_x; ++x) {
                AoiCell* cell =
                    ensure_cells ? EnsureCell(x, y, z) : CellAt(x, y, z);
                if (cell) {
                    fn(*cell);
                }
            }
        }
    }
}

bool AoiSector::HasWatcher(uint64_t watcher_id,
                             const Vector3D& watch_center) const {
    int32_t cx = 0;
    int32_t cy = 0;
    int32_t cz = 0;
    CellIndexAt(watch_center, cx, cy, cz);
    const AoiCell* cell = CellAt(cx, cy, cz);
    return cell && cell->Monitor().HasReceiver(watcher_id);
}

bool AoiSector::AttachWatcher(const EntityPtr& watcher,
                                const Vector3D& watch_center) {
    if (!watcher) {
        return false;
    }
    const uint64_t wid = watcher->GetId();
    if (HasWatcher(wid, watch_center)) {
        return false;
    }
    int32_t cx = 0;
    int32_t cy = 0;
    int32_t cz = 0;
    CellIndexAt(watch_center, cx, cy, cz);
    if (!IsCellIndexInBounds(cx, cy, cz)) {
        return false;
    }
    ForNeighborhood(cx, cy, cz,
                    [&](AoiCell& cell) { cell.AddViewer(watcher); });
    return true;
}

bool AoiSector::RefreshWatcher(const EntityPtr& watcher,
                                 const Vector3D& watch_center) {
    if (!watcher) {
        return false;
    }
    int32_t cx = 0;
    int32_t cy = 0;
    int32_t cz = 0;
    CellIndexAt(watch_center, cx, cy, cz);
    if (!IsCellIndexInBounds(cx, cy, cz)) {
        return false;
    }
    ForNeighborhood(cx, cy, cz,
                    [&](AoiCell& cell) { cell.RefreshViewer(watcher); });
    return true;
}

void AoiSector::DetachWatcher(uint64_t watcher_id,
                                const Vector3D& watch_center, bool notify) {
    int32_t cx = 0;
    int32_t cy = 0;
    int32_t cz = 0;
    CellIndexAt(watch_center, cx, cy, cz);
    if (!IsCellIndexInBounds(cx, cy, cz)) {
        return;
    }
    // ensure_cells=false：离图不创建空 AOI 格
    std::vector<AoiCellKey> touched;
    ForNeighborhood(
        cx, cy, cz,
        [&](AoiCell& cell) {
            cell.RemoveViewer(watcher_id, notify);
            touched.push_back(
                AoiCellKey{cell.CellX(), cell.CellY(), cell.CellZ()});
        },
        false);
    for (const AoiCellKey& key : touched) {
        ReleaseHashCellIfEmpty(key.x, key.y, key.z);
    }
}

bool AoiSector::MoveWatcher(const EntityPtr& watcher,
                              const Vector3D& old_center,
                              const Vector3D& new_center) {
    if (!watcher || !map_) {
        return false;
    }
    if (!map_->IsInMap(new_center) || !map_->IsInMap(old_center)) {
        return false;
    }

    int32_t old_cx = 0;
    int32_t old_cy = 0;
    int32_t old_cz = 0;
    int32_t new_cx = 0;
    int32_t new_cy = 0;
    int32_t new_cz = 0;
    CellIndexAt(old_center, old_cx, old_cy, old_cz);
    CellIndexAt(new_center, new_cx, new_cy, new_cz);
    if (!IsCellIndexInBounds(old_cx, old_cy, old_cz) ||
        !IsCellIndexInBounds(new_cx, new_cy, new_cz)) {
        return false;
    }
    if (old_cx == new_cx && old_cy == new_cy && old_cz == new_cz) {
        return true;
    }

    std::set<AoiCell*> new_cells;
    std::set<AoiCell*> old_cells;
    ForNeighborhood(new_cx, new_cy, new_cz,
                    [&](AoiCell& cell) { new_cells.insert(&cell); }, true);
    ForNeighborhood(old_cx, old_cy, old_cz,
                    [&](AoiCell& cell) { old_cells.insert(&cell); }, false);

    const uint64_t wid = watcher->GetId();
    for (AoiCell* cell : new_cells) {
        if (old_cells.find(cell) == old_cells.end()) {
            cell->AddViewer(watcher);
        }
    }
    std::vector<AoiCellKey> left_keys;
    for (AoiCell* cell : old_cells) {
        if (new_cells.find(cell) == new_cells.end()) {
            left_keys.push_back(
                AoiCellKey{cell->CellX(), cell->CellY(), cell->CellZ()});
            cell->RemoveViewer(wid, true);
        }
    }
    for (const AoiCellKey& key : left_keys) {
        ReleaseHashCellIfEmpty(key.x, key.y, key.z);
    }
    return true;
}

void AoiSector::OnSubjectEnterMap(const EntityPtr& subject) {
    if (!subject) {
        return;
    }
    int32_t cx, cy, cz;
    CellIndexAt(subject->GetPosition(), cx, cy, cz);
    auto it = hash_cells_.find(AoiCellKey{cx, cy, cz});
    bool cell_existed = (it != hash_cells_.end());
    LOG_INFO << "[AOI] OnSubjectEnterMap  id=" << subject->GetId()
             << "  cell=(" << cx << "," << cy << "," << cz << ")"
             << "  cell_existed=" << cell_existed;
    if (AoiCell* cell = EnsureCellAtPosition(subject->GetPosition())) {
        cell->OnSubjectEnter(subject);
    }
}

void AoiSector::OnSubjectLeaveMap(const EntityPtr& subject) {
    if (!subject) {
        return;
    }
    const uint64_t sid = subject->GetId();
    bool removed_home = false;
    if (AoiCell* cell = CellAtPosition(subject->GetPosition())) {
        const bool was_monitoring = cell->Monitor().IsMonitoring(sid);
        cell->OnSubjectLeave(subject);  // Unmonitor(notify=true)
        removed_home = was_monitoring;
    }
    if (!removed_home) {
        // 家格未命中（坐标与 monitor 失步）：全表扫幽灵，命中一处即停
        ForEachCellUntil([sid](AoiCell& cell) {
            return !cell.Monitor().UnmonitorEntity(sid, false);
        });
    }
    // 家格已卸：信任单格足迹 + SwitchMonitor 不留幽灵，跳过 O(cells) 全表
    ReleaseIdleHashCells();
}

void AoiSector::OnSubjectMoved(const EntityPtr& subject,
                                 const Vector3D& old_pos,
                                 const Vector3D& new_pos) {
    if (!subject || !map_) {
        return;
    }
    if (!map_->IsInMap(old_pos) || !map_->IsInMap(new_pos)) {
        return;
    }
    int32_t old_cx = 0;
    int32_t old_cy = 0;
    int32_t old_cz = 0;
    int32_t new_cx = 0;
    int32_t new_cy = 0;
    int32_t new_cz = 0;
    CellIndexAt(old_pos, old_cx, old_cy, old_cz);
    CellIndexAt(new_pos, new_cx, new_cy, new_cz);
    if (old_cx == new_cx && old_cy == new_cy && old_cz == new_cz) {
        return;
    }
    AoiCell* old_cell = EnsureCell(old_cx, old_cy, old_cz);
    AoiCell* new_cell = EnsureCell(new_cx, new_cy, new_cz);
    if (!old_cell || !new_cell) {
        return;
    }
    new_cell->OnSubjectMoved(subject, *old_cell);
    ReleaseHashCellIfEmpty(old_cx, old_cy, old_cz);
}

void AoiSector::FlushDirty(std::vector<EntityPtr>* flushed) {
    if (cell_storage_ != MapGridStorage::kHash) {
        ForEachCell([flushed](AoiCell& cell) { cell.FlushDirty(flushed); });
        return;
    }
    std::vector<AoiCellKey> keys;
    keys.reserve(hash_cells_.size());
    for (const auto& kv : hash_cells_) {
        keys.push_back(kv.first);
    }
    for (const AoiCellKey& key : keys) {
        auto it = hash_cells_.find(key);
        if (it == hash_cells_.end() || !it->second) {
            continue;
        }
        it->second->FlushDirty(flushed);
        ReleaseHashCellIfEmpty(key.x, key.y, key.z);
    }
}
