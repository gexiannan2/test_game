// 世界一致性断言：足迹引用、entity_index_、无重复 ID。
#pragma once

#include <cstdint>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ecs/entity/entity.h"
#include "ecs/systems/world_system.h"
#include "test_harness.h"
#include "test_map_invariants.h"

namespace test {

// 单格足迹：每个在图实体应恰好出现在 1 个格子上。
inline void AssertMapFootprintConsistency(WorldSystem& world,
                                          const std::vector<EntityPtr>& tracked) {
    std::unordered_map<uint64_t, size_t> ref_counts;
    CountEntityRefsOnMap(world.Map(), tracked, ref_counts);

    for (const auto& kv : ref_counts) {
        if (kv.second > 1) {
            std::ostringstream oss;
            oss << "entity " << kv.first << " appears " << kv.second
                << " times on map grids (max 1 for single-cell footprint)";
            Fail(oss.str());
        }
    }

    for (const EntityPtr& e : tracked) {
        if (!e) {
            continue;
        }
        const uint64_t id = e->GetId();
        if (!e->IsInMap()) {
            auto it = ref_counts.find(id);
            if (it != ref_counts.end() && it->second != 0) {
                Fail("off-map entity still referenced in map grids");
            }
            if (world.FindEntity(id) != nullptr) {
                Fail("off-map entity still in registry");
            }
            continue;
        }

        if (world.FindEntity(id) == nullptr) {
            Fail("on-map entity missing from registry");
        }

        auto it = ref_counts.find(id);
        if (it == ref_counts.end() || it->second == 0) {
            Fail("on-map entity missing from map grids");
        }
        if (!FootprintContainsEntity(world.Map(), e->GetPosition(), id)) {
            Fail("on-map entity footprint does not contain entity at position");
        }
    }
}

// 从 tracked 中拆分在图玩家与非玩家主体。
inline void CollectInMapEntities(const std::vector<EntityPtr>& tracked,
                                 std::vector<EntityPtr>& players,
                                 std::vector<EntityPtr>& subjects) {
    players.clear();
    subjects.clear();
    for (const EntityPtr& e : tracked) {
        if (!e || !e->IsInMap()) {
            continue;
        }
        if (e->GetEntityType() == EntityType::kPlayer) {
            players.push_back(e);
        } else {
            subjects.push_back(e);
        }
    }
}

inline void AssertNoDuplicateEntityIds(const std::vector<EntityPtr>& tracked) {
    std::unordered_set<uint64_t> seen;
    for (const EntityPtr& e : tracked) {
        if (!e) {
            continue;
        }
        const uint64_t id = e->GetId();
        if (!seen.insert(id).second) {
            std::ostringstream oss;
            oss << "duplicate entity id in tracked set: " << id;
            Fail(oss.str());
        }
    }
}

// 聚合体检：ID 唯一 + 足迹一致。
inline void AssertWorldConsistent(WorldSystem& world,
                                  const std::vector<EntityPtr>& tracked) {
    AssertNoDuplicateEntityIds(tracked);
    AssertMapFootprintConsistency(world, tracked);
}

}  // namespace test
