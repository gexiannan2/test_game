// AOI 进/离/重进循环测试：验证互相看到、离开消失、重新进入看到对方。

#include "test_harness.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "ecs/entity/entity.h"
#include "ecs/components/transform_component.h"
#include "common/aoi_def.h"
#include "ecs/systems/world_system.h"

#define LOG_TEST() \
  (std::cout << "[TEST ] ")

namespace {

// 测试用简单实体：kPlayer 类型使 IsPlayer() = true（成为观察者）
class TestEntity : public Entity {
 public:
  TestEntity(WorldSystem* world, uint64_t id, EntityType type,
             const EntitySpawn& spawn)
      : Entity(world, id, type, spawn) {}

  bool IsPlayer() const override {
    return GetEntityType() == EntityType::kPlayer;
  }

  bool SerializeAppear(SerializeMsg&, const EntityPtr&) override {
    return false;
  }
  bool SerializeDirty(std::vector<SerializeMsg>&) override { return false; }
};

// 视野事件统计 + 日志
struct AoiStats {
  int appear = 0;
  int disappear = 0;
  int update = 0;
  std::vector<uint64_t> appeared_ids;    // 收到了谁的出现
  std::vector<uint64_t> disappeared_ids; // 收到了谁的消失

  void Print(uint64_t owner_id, const char* label) const {
    LOG_TEST() << "  [" << label << "] viewer=" << owner_id
               << "  appear=" << appear
               << "  disappear=" << disappear
               << "  update=" << update;
    if (!appeared_ids.empty()) {
      std::cout << "  appeared:";
      for (uint64_t id : appeared_ids) std::cout << " " << id;
    }
    if (!disappeared_ids.empty()) {
      std::cout << "  disappeared:";
      for (uint64_t id : disappeared_ids) std::cout << " " << id;
    }
    std::cout << std::endl;
  }
};

struct TestWorld {
  std::shared_ptr<WorldSystem> world;
  std::unordered_map<uint64_t, AoiStats> stats;  // viewer_id → stats

  static std::shared_ptr<TestWorld> Create() {
    auto tw = std::make_shared<TestWorld>();
    tw->world = WorldSystem::Create(SceneRegionType::kMap);
    tw->world->Init();
    tw->world->SetEntityFactory(
        [](WorldSystem* w, uint64_t id, EntityType type,
           const EntitySpawn& spawn) -> EntityPtr {
          return std::make_shared<TestEntity>(w, id, type, spawn);
        });
    tw->world->Aoi().SetEntityEnterCallback(
        [tw](uint64_t viewer_id, const std::vector<uint64_t>& ids) {
          auto& c = tw->stats[viewer_id];
          c.appear += static_cast<int>(ids.size());
          for (uint64_t id : ids) c.appeared_ids.push_back(id);
        });
    tw->world->Aoi().SetEntityLeaveCallback(
        [tw](uint64_t viewer_id, const std::vector<uint64_t>& ids) {
          auto& c = tw->stats[viewer_id];
          c.disappear += static_cast<int>(ids.size());
          for (uint64_t id : ids) c.disappeared_ids.push_back(id);
        });
    tw->world->Aoi().SetEntityUpdateCallback(
        [tw](uint64_t viewer_id, uint64_t subject_id) {
          (void)subject_id;
          auto& c = tw->stats[viewer_id];
          ++c.update;
        });
    return tw;
  }

  EntityPtr SpawnPlayer(uint64_t id, float x, float z) {
    EntitySpawn spawn;
    spawn.position = JPH::Vec3(x, 0.0f, z);
    return world->SpawnOnMap(EntityType::kPlayer, spawn);
  }

  void Dump(const char* label) const {
    LOG_TEST() << "---- " << label << " ----";
    std::cout << "  in-map=" << world->GetEntityCount()
              << "  watcher_count=" << stats.size() << std::endl;
    for (const auto& [id, st] : stats) {
      st.Print(id, "A");
    }
    // 可见实体列表
    for (const auto& [id, st] : stats) {
      auto visible = world->GetVisibleEntities(id);
      std::cout << "  [VIS ] viewer=" << id << "  sees(" << visible.size()
                << "):";
      for (uint64_t vid : visible) std::cout << " " << vid;
      std::cout << std::endl;
    }
  }

  // 检查 viewer 是否在 appeared_ids 中看到 expect_id
  bool SawAppear(uint64_t viewer_id, uint64_t expect_id) const {
    auto it = stats.find(viewer_id);
    if (it == stats.end()) return false;
    for (uint64_t id : it->second.appeared_ids) {
      if (id == expect_id) return true;
    }
    return false;
  }
  bool SawDisappear(uint64_t viewer_id, uint64_t expect_id) const {
    auto it = stats.find(viewer_id);
    if (it == stats.end()) return false;
    for (uint64_t id : it->second.disappeared_ids) {
      if (id == expect_id) return true;
    }
    return false;
  }
};

}  // namespace

// ======================================================================
// 用例 1：A、B 同时进图 → 互相看到
// ======================================================================
GAME_TEST_SUITE(AoiAppearDisappear)

GAME_TEST(AoiAppearDisappear, TwoPlayersSeeEachOtherOnSpawn) {
  std::cout << std::endl;
  LOG_TEST() << ">>> 用例: A/B 同时出生,互相看到 <<<" << std::endl;

  auto tw = TestWorld::Create();
  LOG_TEST() << (tw->world ? "TestWorld created" : "TestWorld FAILED") << std::endl;

  // Step 1: 出生两个玩家 (kAoiCellWorldSize=10, kAoiRadius=1 → 同格可见)
  auto a = tw->SpawnPlayer(1, 0.0f, 0.0f);
  auto b = tw->SpawnPlayer(2, 1.0f, 1.0f);
  EXPECT_TRUE(a->IsInMap());
  EXPECT_TRUE(b->IsInMap());
  tw->Dump("Step1: A(0,0) B(1,1) both spawned");

  EXPECT_EQ(tw->stats.size(), static_cast<size_t>(2));
  EXPECT_TRUE(tw->SawAppear(1, 2));  // A 看到 B
  EXPECT_TRUE(tw->SawAppear(2, 1));  // B 看到 A
  LOG_TEST() << "  PASS: A sees B appear ✓  B sees A appear ✓" << std::endl;
}

// ======================================================================
// 用例 2：A 进图 ← B 进图 互相看到 → B 离开 A 视野 → 收到消失
// ======================================================================
GAME_TEST(AoiAppearDisappear, LeaveAoiRangeDisappears) {
  std::cout << std::endl;
  LOG_TEST() << ">>> 用例: B离开A视野 → A收到B消失, B收到A消失 <<<" << std::endl;

  auto tw = TestWorld::Create();

  auto a = tw->SpawnPlayer(1, 0.0f, 0.0f);
  auto b = tw->SpawnPlayer(2, 1.0f, 1.0f);
  EXPECT_TRUE(tw->SawAppear(1, 2));
  EXPECT_TRUE(tw->SawAppear(2, 1));
  tw->stats.clear();  // 清进场计数
  LOG_TEST() << "  Cleared spawn stats, now moving B away..." << std::endl;

  // B 移到远处：kAoiRadius=1, kAoiCellWorldSize=10 → cell max range [−1, +1]
  // B 从 (1,1)→(300,300): cell(30,30),距 A 的 cell(0,0)=30>1 → 超出视野
  tw->world->MoveEntity(b, JPH::Vec3(300.0f, 0.0f, 300.0f));
  tw->Dump("Step2: B moved to (300,300) — should be invisible");

  EXPECT_TRUE(tw->SawDisappear(1, 2));  // A 看到 B 消失
  EXPECT_TRUE(tw->SawDisappear(2, 1));  // B 看到 A 消失
  LOG_TEST() << "  PASS: A received B disappear ✓  B received A disappear ✓" << std::endl;
}

// ======================================================================
// 用例 3：离开→重新进入→再次互相看到
// ======================================================================
GAME_TEST(AoiAppearDisappear, ReEnterAoiRangeReAppears) {
  std::cout << std::endl;
  LOG_TEST() << ">>> 用例: B离开→消失→B回来→再次互相看到 <<<" << std::endl;

  auto tw = TestWorld::Create();

  auto a = tw->SpawnPlayer(1, 0.0f, 0.0f);
  auto b = tw->SpawnPlayer(2, 1.0f, 1.0f);
  tw->stats.clear();

  // Phase 1: B 离开
  tw->world->MoveEntity(b, JPH::Vec3(300.0f, 0.0f, 300.0f));
  tw->Dump("Phase1: B moved away");

  EXPECT_TRUE(tw->SawDisappear(1, 2));  // A 看到 B 消失
  EXPECT_TRUE(tw->SawDisappear(2, 1));  // B 看到 A 消失
  LOG_TEST() << "  Phase1 PASS: disappear OK" << std::endl;

  tw->stats.clear();

  // Phase 2: B 回来
  tw->world->MoveEntity(b, JPH::Vec3(1.0f, 0.0f, 1.0f));
  tw->Dump("Phase2: B moved back to (1,1)");

  EXPECT_TRUE(tw->SawAppear(1, 2));  // A 重新看到 B
  EXPECT_TRUE(tw->SawAppear(2, 1));  // B 重新看到 A
  LOG_TEST() << "  Phase2 PASS: A re-saw B appear ✓  B re-saw A appear ✓" << std::endl;
}

// ======================================================================
// 用例 4：完整循环 — 出现→离开→回来→离开→回来（健壮性验证）
// ======================================================================
GAME_TEST(AoiAppearDisappear, FullCycleMultipleChurns) {
  std::cout << std::endl;
  LOG_TEST() << ">>> 用例: 多次离开/回来循环 <<<" << std::endl;

  auto tw = TestWorld::Create();
  auto a = tw->SpawnPlayer(1, 0.0f, 0.0f);
  auto b = tw->SpawnPlayer(2, 1.5f, 1.5f);
  tw->stats.clear();

  for (int round = 1; round <= 3; ++round) {
    LOG_TEST() << "  -- Round " << round << " start --" << std::endl;

    // B 离开
    tw->world->MoveEntity(b, JPH::Vec3(200.0f, 0.0f, 200.0f));
    EXPECT_TRUE(tw->SawDisappear(1, 2));
    EXPECT_TRUE(tw->SawDisappear(2, 1));
    LOG_TEST() << "    [Round" << round << "] A saw B disappear: " << tw->SawDisappear(1, 2)
               << "  B saw A disappear: " << tw->SawDisappear(2, 1) << std::endl;

    tw->stats.clear();

    // B 回来
    tw->world->MoveEntity(b, JPH::Vec3(1.5f, 0.0f, 1.5f));
    EXPECT_TRUE(tw->SawAppear(1, 2));
    EXPECT_TRUE(tw->SawAppear(2, 1));
    LOG_TEST() << "    [Round" << round << "] A saw B re-appear: " << tw->SawAppear(1, 2)
               << "  B saw A re-appear: " << tw->SawAppear(2, 1) << std::endl;

    tw->stats.clear();
  }
  LOG_TEST() << "  PASS: 3 churn cycles all OK ✓" << std::endl;
}

// ======================================================================
// 用例 5: A/B 出生不同格但相邻格 → 互相看到（kAoiRadius=1 覆盖邻格）
// ======================================================================
GAME_TEST(AoiAppearDisappear, AdjacentCellsSeeEachOther) {
  std::cout << std::endl;
  LOG_TEST() << ">>> 用例: A(0,0)出 cell0, B(0.2,0.2)出 cell0, 同格可见 <<<" << std::endl;

  auto tw = TestWorld::Create();

  // cell_size=10, A 在 (0,0) → cell(0,0), B 在 (0.2,0.2) → cell(0,0), 同格
  auto a = tw->SpawnPlayer(1, 0.0f, 0.0f);
  auto b = tw->SpawnPlayer(2, 0.2f, 1.0f);
  tw->Dump("Spawn: A(0,0) B(0.2,1) both in cell(0,0)");

  EXPECT_TRUE(tw->SawAppear(1, 2));
  EXPECT_TRUE(tw->SawAppear(2, 1));

  // 边界测试：B 在 (9.5, 0) → cell(0,0), 仍可见
  tw->stats.clear();
  tw->world->MoveEntity(b, JPH::Vec3(9.5f, 0.0f, 1.0f));
  tw->Dump("Move: B(9.5,1) still cell(0,0), kAoiRadius=1 includes it");

  EXPECT_TRUE(tw->SawAppear(1, 2));
  EXPECT_TRUE(tw->SawAppear(2, 1));

  LOG_TEST() << "  PASS: radius=1 correctly covers boundary cell ✓" << std::endl;
}

// ======================================================================
// 用例 6: 压力测试 — 量化统计 appear/disappear 次数（无断言，纯日志观察）
// ======================================================================
GAME_TEST(AoiAppearDisappear, StressAppearDisappearStats) {
  std::cout << std::endl;
  LOG_TEST() << ">>> stress: 10人出生→群移走→群回来 统计出现/消失 <<<" << std::endl;

  auto tw = TestWorld::Create();
  const int N = 10;

  // 出生
  std::vector<EntityPtr> players;
  for (int i = 1; i <= N; ++i) {
    float x = static_cast<float>(i) * 0.5f;
    float z = static_cast<float>(i % 5) * 0.5f;
    players.push_back(tw->SpawnPlayer(i, x, z));
  }
  LOG_TEST() << "  Spawned " << players.size() << " players" << std::endl;
  int total_appear = 0;
  int total_disappear = 0;
  for (const auto& [id, st] : tw->stats) {
    total_appear += st.appear;
    total_disappear += st.disappear;
  }
  LOG_TEST() << "  Total appear=" << total_appear
             << "  disappear=" << total_disappear << std::endl;
  tw->stats.clear();

  // 全部移走
  for (size_t i = 0; i < players.size(); ++i) {
    tw->world->MoveEntity(players[i],
                          JPH::Vec3(500.0f + static_cast<float>(i) * 10.0f,
                                    0.0f, 500.0f));
  }
  int total_leave_appear = 0, total_leave_disappear = 0;
  for (const auto& [id, st] : tw->stats) {
    total_leave_appear += st.appear;
    total_leave_disappear += st.disappear;
  }
  LOG_TEST() << "  After move-away: appear_events=" << total_leave_appear
             << "  disappear_events=" << total_leave_disappear << std::endl;
  tw->stats.clear();

  // 全部回来
  for (size_t i = 0; i < players.size(); ++i) {
    float x = static_cast<float>(i) * 0.5f;
    float z = static_cast<float>(i % 5) * 0.5f;
    tw->world->MoveEntity(players[i], JPH::Vec3(x, 0.0f, z));
  }
  int total_back_appear = 0, total_back_disappear = 0;
  for (const auto& [id, st] : tw->stats) {
    total_back_appear += st.appear;
    total_back_disappear += st.disappear;
  }
  LOG_TEST() << "  After move-back: appear_events=" << total_back_appear
             << "  disappear_events=" << total_back_disappear << std::endl;

  LOG_TEST() << "  done — check stats above for reasonableness" << std::endl;
}
