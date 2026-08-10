// AOI 可见性 Oracle：按邻域半径独立推算，与 VisibilityModel 对拍。
#pragma once

#include <cstdint>
#include <cmath>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/vector3d.h"
#include "common/aoi_def.h"
#include "ecs/systems/aoi_sector.h"
#include "ecs/systems/world_system.h"
#include "test_harness.h"
#include "test_map_invariants.h"

namespace test {
namespace aoi_oracle {

using test::GridCenter;
using test::MakeWorld;

using VisPair = std::pair<uint64_t, uint64_t>;

// 在指定逻辑格中心生成并进图（默认 kMarch，可传 kPlayer）
inline EntityPtr SpawnOnGrid(const std::shared_ptr<WorldSystem>& world,
                             uint32_t gx, uint32_t gy,
                             EntityType type = EntityType::kMarch) {
    return world->SpawnOnMap(type, EntitySpawn::At(GridCenter(gx, gy)));
}

inline int32_t ViewCellIndex(int32_t grid_coord, uint32_t cell_size_grids) {
    return static_cast<int32_t>(
        std::floor(static_cast<float>(grid_coord) /
                   static_cast<float>(cell_size_grids)));
}

// 无界地图下 max_cell 设为 INT32_MAX（不裁剪）
// 根据观察者 GetGridCenter 计算其所在 AOI 视野格索引
inline void WatchCellForPlayer(const WorldSystem& /*world*/, const EntityPtr& player,
                               int32_t& cx, int32_t& cy, int32_t& cz) {
    const Vector3D center = player->GetGridCenter();
    cx = WorldToAoiCellIndex(center.GetX());
    cy = WorldToAoiCellIndex(center.GetY());
    cz = WorldToAoiCellIndex(center.GetZ());
}

inline void SubjectCell(const EntityPtr& subject, int32_t& cx, int32_t& cy,
                        int32_t& cz) {
    const Vector3D pos = subject->GetPosition();
    cx = pos.AoiCellX();
    cy = pos.AoiCellY();
    cz = pos.AoiCellZ();
}

// 无界地图：不裁剪到 max_cell，仅按半径判断
// Oracle 判定：被观察者 AOI 格在观察者邻域 ±kNeighborhoodRadius 内则可见
inline bool OracleWatcherSeesSubject(int32_t watch_cx, int32_t watch_cy,
                                     int32_t watch_cz, int32_t sub_cx,
                                     int32_t sub_cy, int32_t sub_cz) {
    return std::abs(sub_cx - watch_cx) <= kNeighborhoodRadius &&
           std::abs(sub_cy - watch_cy) <= kNeighborhoodRadius &&
           std::abs(sub_cz - watch_cz) <= kNeighborhoodRadius;
}

// 计算所有在图 watcher×subject 的可见对集合（不含自己看自己）
inline std::set<VisPair> ComputeOracleVisiblePairs(
    const WorldSystem& world, const std::vector<EntityPtr>& watchers,
    const std::vector<EntityPtr>& subjects,
    int32_t /*max_cx*/ = 0, int32_t /*max_cy*/ = 0, int32_t /*max_cz*/ = 0) {
    std::set<VisPair> pairs;
    for (const EntityPtr& player : watchers) {
        if (!player || !player->IsInMap()) {
            continue;
        }
        int32_t wcx = 0, wcy = 0, wcz = 0;
        WatchCellForPlayer(world, player, wcx, wcy, wcz);
        const uint64_t wid = player->GetId();
        for (const EntityPtr& subject : subjects) {
            if (!subject || !subject->IsInMap()) {
                continue;
            }
            const uint64_t sid = subject->GetId();
            if (sid == wid) {
                continue;
            }
            int32_t scx = 0, scy = 0, scz = 0;
            SubjectCell(subject, scx, scy, scz);
            if (OracleWatcherSeesSubject(wcx, wcy, wcz, scx, scy, scz)) {
                pairs.insert(VisPair{wid, sid});
            }
        }
    }
    return pairs;
}

inline void DiffVisiblePairs(const std::set<VisPair>& before,
                             const std::set<VisPair>& after,
                             std::set<VisPair>& added,
                             std::set<VisPair>& removed) {
    added.clear();
    removed.clear();
    for (const VisPair& p : after) {
        if (before.find(p) == before.end()) {
            added.insert(p);
        }
    }
    for (const VisPair& p : before) {
        if (after.find(p) == after.end()) {
            removed.insert(p);
        }
    }
}

inline std::vector<EntityPtr> AllOnMapSubjects(
    const std::vector<EntityPtr>& players,
    const std::vector<EntityPtr>& npcs) {
    std::vector<EntityPtr> all = npcs;
    for (const EntityPtr& p : players) {
        all.push_back(p);
    }
    return all;
}

// 根据 AOI enter/leave 回调维护的「谁看见谁」内存模型（测试替身，非生产代码）
class VisibilityModel {
 public:
    void OnEntityEnter(uint64_t viewer_entity_id,
                       const std::vector<uint64_t>& subject_entity_ids) {
        for (uint64_t sid : subject_entity_ids) {
            if (viewer_entity_id == sid) {
                continue;
            }
            visible_[viewer_entity_id].insert(sid);
        }
    }

    void OnEntityLeave(uint64_t viewer_entity_id,
                       const std::vector<uint64_t>& subject_entity_ids) {
        auto it = visible_.find(viewer_entity_id);
        if (it == visible_.end()) {
            return;
        }
        for (uint64_t sid : subject_entity_ids) {
            it->second.erase(sid);
        }
    }

    void OnEntityUpdate(uint64_t, uint64_t) {}

    bool Sees(uint64_t viewer, uint64_t subject) const {
        auto it = visible_.find(viewer);
        if (it == visible_.end()) {
            return false;
        }
        return it->second.count(subject) != 0;
    }

    const std::unordered_set<uint64_t>& VisibleSet(uint64_t viewer) const {
        static const std::unordered_set<uint64_t> kEmpty;
        auto it = visible_.find(viewer);
        return it == visible_.end() ? kEmpty : it->second;
    }

    void Clear() { visible_.clear(); }
    void ClearViewer(uint64_t viewer_id) { visible_.erase(viewer_id); }

 private:
    std::unordered_map<uint64_t, std::unordered_set<uint64_t>> visible_;
};

// 逐步统计每个 (viewer, subject) 的 enter/leave/update 次数（本步 + 累计）
class PerViewerNotifyStats {
 public:
    void ClearStep() {
        enter_step_.clear();
        leave_step_.clear();
        update_step_.clear();
    }

    void OnEnter(uint64_t viewer, uint64_t subject) {
        if (viewer == subject) return;
        ++enter_step_[viewer][subject];
        ++enter_total_[viewer][subject];
    }
    void OnLeave(uint64_t viewer, uint64_t subject) {
        if (viewer == subject) return;
        ++leave_step_[viewer][subject];
        ++leave_total_[viewer][subject];
    }
    void OnUpdate(uint64_t viewer, uint64_t subject) {
        if (viewer == subject) return;
        ++update_step_[viewer][subject];
        ++update_total_[viewer][subject];
    }

    uint32_t LeaveStepCount(uint64_t viewer, uint64_t subject) const {
        return CountIn(leave_step_, viewer, subject);
    }
    uint32_t EnterStepCount(uint64_t viewer, uint64_t subject) const {
        return CountIn(enter_step_, viewer, subject);
    }
    uint32_t UpdateStepCount(uint64_t viewer, uint64_t subject) const {
        return CountIn(update_step_, viewer, subject);
    }
    uint32_t LeaveTotalCount(uint64_t viewer, uint64_t subject) const {
        return CountIn(leave_total_, viewer, subject);
    }

 private:
    static uint32_t CountIn(
        const std::unordered_map<uint64_t,
                                 std::unordered_map<uint64_t, uint32_t>>& m,
        uint64_t viewer, uint64_t subject) {
        auto vit = m.find(viewer);
        if (vit == m.end()) return 0;
        auto sit = vit->second.find(subject);
        return sit == vit->second.end() ? 0 : sit->second;
    }

    std::unordered_map<uint64_t, std::unordered_map<uint64_t, uint32_t>>
        enter_step_;
    std::unordered_map<uint64_t, std::unordered_map<uint64_t, uint32_t>>
        leave_step_;
    std::unordered_map<uint64_t, std::unordered_map<uint64_t, uint32_t>>
        update_step_;
    std::unordered_map<uint64_t, std::unordered_map<uint64_t, uint32_t>>
        enter_total_;
    std::unordered_map<uint64_t, std::unordered_map<uint64_t, uint32_t>>
        leave_total_;
    std::unordered_map<uint64_t, std::unordered_map<uint64_t, uint32_t>>
        update_total_;
};

// 组合 VisibilityModel + PerViewerNotifyStats + 广播计数，供 mass/broadcast 测试挂载
class BroadcastAuditSync {
 public:
    VisibilityModel model;
    PerViewerNotifyStats per_viewer;
    size_t enter_subject_refs = 0;
    size_t leave_subject_refs = 0;
    size_t update_events = 0;

    void ResetCounters() {
        enter_subject_refs = 0;
        leave_subject_refs = 0;
        update_events = 0;
        per_viewer.ClearStep();
    }

    void OnEntityEnter(uint64_t viewer_entity_id,
                       const std::vector<uint64_t>& subject_entity_ids) {
        model.OnEntityEnter(viewer_entity_id, subject_entity_ids);
        for (uint64_t sid : subject_entity_ids) {
            if (sid != viewer_entity_id) {
                ++enter_subject_refs;
                per_viewer.OnEnter(viewer_entity_id, sid);
            }
        }
    }

    void OnEntityLeave(uint64_t viewer_entity_id,
                       const std::vector<uint64_t>& subject_entity_ids) {
        model.OnEntityLeave(viewer_entity_id, subject_entity_ids);
        for (uint64_t sid : subject_entity_ids) {
            if (sid != viewer_entity_id) {
                ++leave_subject_refs;
                per_viewer.OnLeave(viewer_entity_id, sid);
            }
        }
    }

    void OnEntityUpdate(uint64_t viewer_entity_id, uint64_t subject_entity_id) {
        ++update_events;
        per_viewer.OnUpdate(viewer_entity_id, subject_entity_id);
        model.OnEntityUpdate(viewer_entity_id, subject_entity_id);
    }

    void SyncWatchersWhoLeftMap(
        const std::vector<EntityPtr>& players,
        const std::set<uint64_t>& players_on_map_before) {
        for (const EntityPtr& p : players) {
            if (!p) continue;
            const uint64_t id = p->GetId();
            if (players_on_map_before.count(id) && !p->IsInMap()) {
                model.ClearViewer(id);
            }
        }
    }

    void SyncWatcherBeforeReenter(uint64_t viewer_id, bool on_map) {
        if (!on_map) {
            model.ClearViewer(viewer_id);
        }
    }
};

// 断言：离图 subject 对每个 oracle 可见的 watcher 恰好收到 expected 次 leave 通知
inline void AssertWatchersLeaveNotifyForSubject(
    const WorldSystem& world, const std::vector<EntityPtr>& watchers,
    const PerViewerNotifyStats& stats, const EntityPtr& subject,
    int32_t /*max_cx*/, int32_t /*max_cy*/, int32_t /*max_cz*/,
    uint32_t expected_per_watcher, const char* context) {
    if (!subject) Fail("AssertWatchersLeaveNotifyForSubject: null subject");
    int32_t scx = 0, scy = 0, scz = 0;
    SubjectCell(subject, scx, scy, scz);
    const uint64_t sid = subject->GetId();
    size_t oracle_watchers = 0;
    for (const EntityPtr& player : watchers) {
        if (!player || !player->IsInMap()) continue;
        const uint64_t wid = player->GetId();
        if (wid == sid) continue;
        int32_t wcx = 0, wcy = 0, wcz = 0;
        WatchCellForPlayer(world, player, wcx, wcy, wcz);
        const bool oracle = OracleWatcherSeesSubject(wcx, wcy, wcz, scx, scy, scz);
        const uint32_t got = stats.LeaveStepCount(wid, sid);
        if (oracle) {
            ++oracle_watchers;
            if (got != expected_per_watcher) {
                std::ostringstream oss;
                oss << context << " watcher=" << wid << " subject=" << sid
                    << " expected_leave=" << expected_per_watcher << " got=" << got;
                Fail(oss.str());
            }
        } else if (got != 0) {
            std::ostringstream oss;
            oss << context << " unexpected leave watcher=" << wid << " subject=" << sid << " got=" << got;
            Fail(oss.str());
        }
    }
    if (oracle_watchers == 0) {
        std::ostringstream oss;
        oss << context << " no oracle watchers for subject=" << sid;
        Fail(oss.str());
    }
}

// 断言：进图 subject 对每个 oracle 可见的 watcher 恰好收到 expected 次 enter 通知
inline void AssertWatchersEnterNotifyForSubject(
    const WorldSystem& world, const std::vector<EntityPtr>& watchers,
    const PerViewerNotifyStats& stats, const EntityPtr& subject,
    int32_t /*max_cx*/, int32_t /*max_cy*/, int32_t /*max_cz*/,
    uint32_t expected_per_watcher, const char* context) {
    if (!subject) Fail("AssertWatchersEnterNotifyForSubject: null subject");
    int32_t scx = 0, scy = 0, scz = 0;
    SubjectCell(subject, scx, scy, scz);
    const uint64_t sid = subject->GetId();
    for (const EntityPtr& player : watchers) {
        if (!player || !player->IsInMap()) continue;
        const uint64_t wid = player->GetId();
        if (wid == sid) continue;
        int32_t wcx = 0, wcy = 0, wcz = 0;
        WatchCellForPlayer(world, player, wcx, wcy, wcz);
        const bool oracle = OracleWatcherSeesSubject(wcx, wcy, wcz, scx, scy, scz);
        const uint32_t got = stats.EnterStepCount(wid, sid);
        if (oracle) {
            if (got != expected_per_watcher) {
                std::ostringstream oss;
                oss << context << " watcher=" << wid << " subject=" << sid
                    << " expected_enter=" << expected_per_watcher << " got=" << got;
                Fail(oss.str());
            }
        } else if (got != 0) {
            std::ostringstream oss;
            oss << context << " unexpected enter watcher=" << wid << " subject=" << sid << " got=" << got;
            Fail(oss.str());
        }
    }
}

// 双向校验：model 与 oracle 可见性一致，且 model 无离图/超距残留
inline void AssertModelMatchesOracle(
    const WorldSystem& world, VisibilityModel& model,
    const std::vector<EntityPtr>& players,
    const std::vector<EntityPtr>& subjects,
    int32_t /*max_cx*/ = 0, int32_t /*max_cy*/ = 0, int32_t /*max_cz*/ = 0,
    const char* context = "") {
    for (const EntityPtr& player : players) {
        if (!player || !player->IsInMap()) continue;
        int32_t wcx = 0, wcy = 0, wcz = 0;
        WatchCellForPlayer(world, player, wcx, wcy, wcz);
        const uint64_t wid = player->GetId();

        for (const EntityPtr& subject : subjects) {
            if (!subject || !subject->IsInMap()) continue;
            const uint64_t sid = subject->GetId();
            if (sid == wid) continue;
            int32_t scx = 0, scy = 0, scz = 0;
            SubjectCell(subject, scx, scy, scz);
            const bool oracle = OracleWatcherSeesSubject(wcx, wcy, wcz, scx, scy, scz);
            const bool model_sees = model.Sees(wid, sid);
            if (oracle != model_sees) {
                std::ostringstream oss;
                oss << context << " watcher=" << wid << " subject=" << sid
                    << " oracle=" << oracle << " model=" << model_sees;
                Fail(oss.str());
            }
        }
    }

    for (const EntityPtr& player : players) {
        if (!player || !player->IsInMap()) continue;
        const uint64_t wid = player->GetId();
        for (uint64_t sid : model.VisibleSet(wid)) {
            if (sid == wid) continue;
            EntityPtr subject = world.FindEntity(sid);
            if (!subject || !subject->IsInMap()) {
                std::ostringstream oss;
                oss << context << " stale visibility watcher=" << wid
                    << " subject=" << sid << " (off map)";
                Fail(oss.str());
            }
            int32_t scx = 0, scy = 0, scz = 0;
            SubjectCell(subject, scx, scy, scz);
            int32_t wcx = 0, wcy = 0, wcz = 0;
            WatchCellForPlayer(world, player, wcx, wcy, wcz);
            if (!OracleWatcherSeesSubject(wcx, wcy, wcz, scx, scy, scz)) {
                std::ostringstream oss;
                oss << context << " stale visibility watcher=" << wid
                    << " subject=" << sid << " (out of range)";
                Fail(oss.str());
            }
        }
    }
}

inline size_t CountLeaveBroadcastRefs(const std::set<VisPair>& removed,
                                      const WorldSystem& world) {
    size_t n = 0;
    for (const VisPair& p : removed) {
        if (p.first == p.second) continue;
        EntityPtr watcher = world.FindEntity(p.first);
        if (watcher && watcher->IsInMap() && watcher->NeedsAoiWatcher()) {
            ++n;
        }
    }
    return n;
}

// 断言本步 enter/leave 回调次数与 oracle 可见边增删数量一致
inline void AssertBroadcastDeltaMatchesOracle(
    size_t enter_refs, size_t leave_refs, const std::set<VisPair>& added,
    const std::set<VisPair>& removed, const WorldSystem& world,
    const char* context) {
    (void)world;
    if (enter_refs != added.size()) {
        std::ostringstream oss;
        oss << context << " enter broadcast refs=" << enter_refs
            << " oracle_new_pairs=" << added.size();
        Fail(oss.str());
    }
    const size_t expected_leave = CountLeaveBroadcastRefs(removed, world);
    if (leave_refs != expected_leave) {
        std::ostringstream oss;
        oss << context << " leave broadcast refs=" << leave_refs
            << " oracle_leave_broadcast_pairs=" << expected_leave
            << " visibility_removed_pairs=" << removed.size();
        Fail(oss.str());
    }
}

// 可复现伪随机（xorshift*），用于 churn 测试选操作与目标
struct Rng {
    explicit Rng(uint64_t seed) : state_(seed ? seed : 1) {}
    uint32_t NextU32() {
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return static_cast<uint32_t>((state_ * 0x2545F4914F6CDD1DULL) >> 32);
    }
    uint32_t Range(uint32_t hi_exclusive) {
        return hi_exclusive == 0 ? 0 : NextU32() % hi_exclusive;
    }

 private:
    uint64_t state_;
};

}  // namespace aoi_oracle
}  // namespace test
