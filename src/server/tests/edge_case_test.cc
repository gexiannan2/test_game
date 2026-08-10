// edge_case_test.cc — 异常/边界：AOI 重入离图、绕障几何、移动打断、脏标记
#include <list>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "ecs/components/move_component.h"
#include "ecs/systems/map_system.h"
#include "test_bind_callbacks.h"
#include "test_harness.h"
#include "test_map_invariants.h"
#include "zrpc/base/logger.h"

using test::GridCenter;
using test::MakeWorld;

GAME_TEST_SUITE(EdgeCaseTest);

namespace {

struct CountingSceneSync {
  void OnEntityEnter(uint64_t, const std::vector<uint64_t>&) {}
  void OnEntityLeave(uint64_t, const std::vector<uint64_t>&) {}
  void OnEntityUpdate(uint64_t, uint64_t) {}
};

struct ReentrantLeaveSync {
  WorldSystem* world = nullptr;
  EntityPtr victim;
  std::atomic<int> leave_events{0};
  std::atomic<int> nested_leaves{0};

  void OnEntityEnter(uint64_t, const std::vector<uint64_t>&) {}
  void OnEntityUpdate(uint64_t, uint64_t) {}
  void OnEntityLeave(uint64_t, const std::vector<uint64_t>&) {
    leave_events.fetch_add(1);
    if (victim && world && victim->IsInMap()) {
      nested_leaves.fetch_add(1);
      world->LeaveMap(victim);
    }
  }
};

}  // namespace

// AOI leave 回调内再 LeaveMap 另一实体：不得崩溃（对齐 Send→HandleClose 重入）
GAME_TEST(EdgeCaseTest, AoiLeaveCallbackNestedLeaveMapSafe) {
  auto world = MakeWorld();
  auto sync = std::make_shared<ReentrantLeaveSync>();
  sync->world = world.get();
  test::BindAoiCallbacks(*world, sync);

  EntityPtr a = world->SpawnOnMap(EntityType::kPlayer,
                                  EntitySpawn::At(GridCenter(10, 10)));
  EntityPtr b = world->SpawnOnMap(EntityType::kPlayer,
                                  EntitySpawn::At(GridCenter(10, 10)));
  EntityPtr c = world->SpawnOnMap(EntityType::kPlayer,
                                  EntitySpawn::At(GridCenter(10, 10)));
  sync->victim = c;
  world->LeaveMap(a);
  EXPECT_FALSE(a->IsInMap());
  EXPECT_FALSE(c->IsInMap());
  EXPECT_TRUE(b->IsInMap());
  EXPECT_GT(sync->leave_events.load(), 0);
  EXPECT_GT(sync->nested_leaves.load(), 0);
  world->LeaveMap(b);
}

// 连续 LeaveMap 幂等 + 离图后脏标记不崩
GAME_TEST(EdgeCaseTest, DirtyAfterLeaveMapNoCrash) {
  auto world = MakeWorld();
  EntityPtr e = world->SpawnOnMap(EntityType::kPlayer,
                                  EntitySpawn::At(GridCenter(4, 4)));
  world->LeaveMap(e);
  world->LeaveMap(e);
  e->SetPropertyDirty(EntityPropertyType::kMove);
  e->SetPropertyDirty(EntityPropertyType::kStopMove);
  EXPECT_FALSE(e->IsInMap());
}

// 三次换目标：最终停在最后目的地附近，不卡在 IsMoving
GAME_TEST(EdgeCaseTest, TripleRetargetMoveCompletesLast) {
  auto world = MakeWorld();
  EntityPtr m = world->SpawnOnMap(EntityType::kMarch,
                                  EntitySpawn::At(GridCenter(5, 5)));
  world->Move().SetMoveSpeed(m, 200.f);
  EXPECT_TRUE(world->Move().RequestMoveTo(m, GridCenter(40, 5), nullptr));
  EXPECT_TRUE(world->Move().RequestMoveTo(m, GridCenter(5, 40), nullptr));
  const Vector3D last = GridCenter(20, 20);
  EXPECT_TRUE(world->Move().RequestMoveTo(m, last, nullptr));
  for (int i = 0; i < 5000 && world->Move().IsMoving(m); ++i) {
    world->Tick();
  }
  EXPECT_FALSE(world->Move().IsMoving(m));
  const Vector3D p = m->GetPosition();
  EXPECT_NEAR(p.GetX(), last.GetX(), 2.f);
  EXPECT_NEAR(p.GetZ(), last.GetZ(), 2.f);
  world->LeaveMap(m);
}

// 移动中途改零速：请求仍接受，Tick 后停止
GAME_TEST(EdgeCaseTest, SpeedZeroMidMoveStops) {
  auto world = MakeWorld();
  EntityPtr m = world->SpawnOnMap(EntityType::kMarch,
                                  EntitySpawn::At(GridCenter(6, 6)));
  world->Move().SetMoveSpeed(m, 80.f);
  bool stopped = false;
  MoveStopReason reason = MoveStopReason::kSuccess;
  EXPECT_TRUE(world->Move().RequestMoveTo(
      m, GridCenter(50, 6),
      [&](const EntityPtr&, bool ok, MoveStopReason r) {
        stopped = true;
        reason = r;
        EXPECT_FALSE(ok);
      }));
  world->Tick();
  EXPECT_TRUE(world->Move().IsMoving(m));
  world->Move().SetMoveSpeed(m, 0.f);
  for (int i = 0; i < 10; ++i) world->Tick();
  EXPECT_TRUE(stopped);
  EXPECT_EQ(reason, MoveStopReason::kZeroSpeed);
  EXPECT_FALSE(world->Move().IsMoving(m));
  world->LeaveMap(m);
}

// 绕球：零半径 / 共线短向量 / 正常绕障不崩
GAME_TEST(EdgeCaseTest, SpherePathEdgeGeometries) {
  MapSystem map(SceneRegionType::kMap);
  map.Init();
  Vector3D start(0.f, 0.f, 0.f);
  Vector3D target(100.f, 0.f, 0.f);
  Vector3D center(50.f, 0.f, 0.f);
  Vector3D cvt;
  EXPECT_FALSE(map.IsRelationEntity(start, target, center, 0.f, cvt));

  std::list<Vector3D> paths;
  EXPECT_TRUE(map.CalcMoveSpherePath(start, target, center, 0.f, paths) ==
              PathCalcResult::kNone);

  // 终点在球内 → PopTarget 或 None（实现分支）
  paths.clear();
  auto r = map.CalcMoveSpherePath(start, Vector3D(50.f, 0.f, 0.f), center, 10.f,
                                  paths);
  EXPECT_TRUE(r == PathCalcResult::kPopTarget || r == PathCalcResult::kNone ||
              r == PathCalcResult::kMoveToTarget);

  paths.clear();
  r = map.CalcMoveSpherePath(start, target, center, 15.f, paths);
  EXPECT_TRUE(r == PathCalcResult::kMoveToTarget ||
              r == PathCalcResult::kNone || r == PathCalcResult::kPopTarget);
}

// 城池在路径上：行军仍能完成（回归 MoveCompletesNearObstacle）
GAME_TEST(EdgeCaseTest, MarchPastTownCompletes) {
  auto world = MakeWorld(64, 64);
  EntitySpawn town_spawn = EntitySpawn::At(GridCenter(12, 8));
  town_spawn.scale = kGridSize * 2;
  town_spawn.collision = kGridSize;
  world->SpawnOnMap(EntityType::kTown, town_spawn);
  EntityPtr march = world->SpawnOnMap(EntityType::kMarch,
                                      EntitySpawn::At(GridCenter(8, 8)));
  world->Move().SetMoveSpeed(march, 250.f);
  bool done = false;
  world->Move().RequestMoveTo(
      march, GridCenter(16, 8),
      [&](const EntityPtr&, bool ok, MoveStopReason) {
        EXPECT_TRUE(ok);
        done = true;
      });
  for (int i = 0; i < 4000 && !done; ++i) world->Tick();
  EXPECT_TRUE(done);
  world->LeaveMap(march);
}

// 同格挤满后集体跨 AOI 粗格：不崩且离图干净
GAME_TEST(EdgeCaseTest, MassSameCellThenCrossAoiCell) {
  auto world = MakeWorld();
  auto sync = std::make_shared<CountingSceneSync>();
  test::BindAoiCallbacks(*world, sync);
  constexpr int kN = 64;
  std::vector<EntityPtr> players;
  players.reserve(kN);
  for (int i = 0; i < kN; ++i) {
    players.push_back(world->SpawnOnMap(
        EntityType::kPlayer, EntitySpawn::At(GridCenter(20, 20))));
  }
  LOG_INFO << "[TEST] after spawn: grid_count=" << world->Map().AllocatedGridCount();

  for (auto& p : players) {
    world->MoveEntity(p, GridCenter(20 + 40, 20));  // 跨多个 AOI 粗格
  }
  LOG_INFO << "[TEST] after move:  grid_count=" << world->Map().AllocatedGridCount();

  for (auto& p : players) {
    world->LeaveMap(p);
    LOG_INFO << "[TEST] after leave grid_count=" << world->Map().AllocatedGridCount();
  }
  LOG_INFO << "[TEST] final grid_count=" << world->Map().AllocatedGridCount();
  EXPECT_EQ(world->Map().AllocatedGridCount(), 0u);
}
