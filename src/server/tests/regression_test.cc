// 历史回归：双 EnterMap、离图清注册表、AOI 回调绑定、寻路绕障等。

#include <cstdint>
#include <memory>
#include <vector>

#include "ecs/entity/entity.h"
#include "common/aoi_def.h"
#include "ecs/components/move_component.h"
#include "ecs/systems/world_system.h"
#include "ecs/systems/map_system.h"
#include "test_bind_callbacks.h"
#include "test_harness.h"
#include "test_map_invariants.h"

namespace {
using test::MakeDefaultEntityFactory;

Vector3D GridCenter(uint32_t gx, uint32_t gy, uint32_t gz = 0) {
  return Vector3D(MapGridIndexToCenterWorld(static_cast<int32_t>(gx)),
                  MapGridIndexToCenterWorld(static_cast<int32_t>(gy)),
                  MapGridIndexToCenterWorld(static_cast<int32_t>(gz)));
}

std::shared_ptr<WorldSystem> MakeWorld(uint32_t w, uint32_t h, uint32_t d = 8) {
  auto world = WorldSystem::Create(SceneRegionType::kMap);
  world->SetEntityFactory(MakeDefaultEntityFactory());
  world->Init();
  return world;
}

bool GridContainsEntity(MapSystem& map, int32_t gx, int32_t gy, uint64_t id,
                        int32_t gz = 0) {
  MapGrid* grid = map.GetGrid(gx, gy, gz);
  if (grid == nullptr) {
    return false;
  }
  for (const auto& [eid, e] : grid->GetEntities()) {
    (void)eid;
    if (e && e->GetId() == id) {
      return true;
    }
  }
  return false;
}

struct CountingSceneSync {
  size_t enter = 0;
  void OnEntityEnter(uint64_t, const std::vector<uint64_t>&) { ++enter; }
  void OnEntityLeave(uint64_t, const std::vector<uint64_t>&) {}
  void OnEntityUpdate(uint64_t, uint64_t) {}
};

bool FootprintContainsEntity(MapSystem& map, const Vector3D& pos, uint64_t id) {
  return GridContainsEntity(map, pos.GridX(), pos.GridY(), id, pos.GridZ());
}

}  // namespace

GAME_TEST_SUITE(RegressionTest);

GAME_TEST(RegressionTest, DoubleEnterMapIsIdempotent) {
  auto world = MakeWorld(32, 32);
  EntityPtr e = world->Spawn(EntityType::kMarch,
                             EntitySpawn::At(GridCenter(5, 5)));
  world->EnterMap(e);
  world->EnterMap(e);
  EXPECT_TRUE(e->IsInMap());
  EXPECT_NE(world->FindEntity(e->GetId()), nullptr);
}

GAME_TEST(RegressionTest, RequestMoveRequiresInMap) {
  auto world = MakeWorld(32, 32);
  EntityPtr e = world->Spawn(EntityType::kMarch,
                             EntitySpawn::At(GridCenter(3, 3)));
  bool ok = world->Move().RequestMoveTo(e, GridCenter(5, 3), nullptr);
  EXPECT_FALSE(ok);
  world->EnterMap(e);
  ok = world->Move().RequestMoveTo(e, GridCenter(5, 3), nullptr);
  EXPECT_TRUE(ok);
  world->LeaveMap(e);
}

GAME_TEST(RegressionTest, RegistryFindAfterLeaveMap) {
  auto world = MakeWorld(32, 32);
  EntityPtr e = world->SpawnOnMap(EntityType::kMarch,
                                  EntitySpawn::At(GridCenter(2, 2)));
  const uint64_t id = e->GetId();
  world->LeaveMap(e);
  EXPECT_EQ(world->FindEntity(id), nullptr);
  EXPECT_FALSE(e->IsInMap());
}

// Init 前绑回调：Init 末尾 RebuildViewNotify 后仍应收到 appear
GAME_TEST(RegressionTest, BindAoiCallbacksBeforeInitStillReceivesEvents) {
  auto world = WorldSystem::Create(SceneRegionType::kMap);
  world->SetEntityFactory(MakeDefaultEntityFactory());
  auto sync = std::make_shared<CountingSceneSync>();
  test::BindAoiCallbacks(*world, sync);
  world->Init();

  world->SpawnOnMap(EntityType::kPlayer,
                    EntitySpawn::At(GridCenter(4, 4)));
  world->SpawnOnMap(EntityType::kMarch,
                    EntitySpawn::At(GridCenter(3, 4)));
  EXPECT_GE(sync->enter, 1u);
}

GAME_TEST(RegressionTest, BindAoiCallbacksAfterInitReceivesEvents) {
  auto world = WorldSystem::Create(SceneRegionType::kMap);
  world->SetEntityFactory(MakeDefaultEntityFactory());
  world->Init();
  auto sync = std::make_shared<CountingSceneSync>();
  test::BindAoiCallbacks(*world, sync);

  // 同 AOI 视野格（格中心世界坐标落在同一 10m 立方内）
  world->SpawnOnMap(EntityType::kPlayer,
                    EntitySpawn::At(GridCenter(4, 4)));
  world->SpawnOnMap(EntityType::kMarch,
                    EntitySpawn::At(GridCenter(3, 4)));
  EXPECT_GE(sync->enter, 1u);
}

GAME_TEST(RegressionTest, MapFootprintMoveClearsOldGrids) {
  auto world = MakeWorld(48, 48);
  EntityPtr unit = world->SpawnOnMap(EntityType::kMarch,
                                     EntitySpawn::At(GridCenter(12, 10)));
  const uint64_t unit_id = unit->GetId();
  const Vector3D old_pos = unit->GetPosition();
  MapSystem& map = world->Map();
  EXPECT_TRUE(unit->IsInMap());
  EXPECT_TRUE(FootprintContainsEntity(map, old_pos, unit_id));

  world->MoveEntity(unit, GridCenter(20, 10));
  EXPECT_FALSE(FootprintContainsEntity(map, old_pos, unit_id));
  EXPECT_TRUE(FootprintContainsEntity(map, unit->GetPosition(), unit_id));
}

GAME_TEST(RegressionTest, LeaveMapWhileMovingClearsDirtyBits) {
  auto world = MakeWorld(32, 32);
  EntityPtr e = world->SpawnOnMap(EntityType::kMarch,
                                  EntitySpawn::At(GridCenter(4, 4)));
  EXPECT_TRUE(world->Move().RequestMoveTo(e, GridCenter(8, 4), nullptr));
  EXPECT_TRUE(e->Move() && e->Move()->IsMoving());
  world->LeaveMap(e);
  EXPECT_FALSE(e->IsInMap());
  EXPECT_FALSE(e->IsDirty());
}

GAME_TEST(RegressionTest, DirtyUpdateAfterLeaveDoesNotCrash) {
  auto world = MakeWorld(32, 32);
  test::BindAoiCallbacks(*world, std::make_shared<CountingSceneSync>());
  EntityPtr player = world->SpawnOnMap(EntityType::kPlayer,
                                         EntitySpawn::At(GridCenter(6, 6)));
  EntityPtr town = world->Spawn(EntityType::kTown,
                                EntitySpawn::At(GridCenter(7, 6)));
  world->EnterMap(town);
  // 离图前标脏并 Tick，覆盖「脏队列残留 + LeaveMap」路径，防止悬空更新崩溃
  if (auto te = town) {
    te->SetPropertyDirty(EntityPropertyType::kMove, true);
  }
  world->Tick();
  world->LeaveMap(town);
  town.reset();
  world->Tick();
  (void)player;
}

GAME_TEST(RegressionTest, MoveCompletesNearObstacle) {
  auto world = MakeWorld(48, 48);
  world->SpawnOnMap(EntityType::kPlayer,
                    EntitySpawn::At(GridCenter(4, 4)));
  EntitySpawn town_spawn = EntitySpawn::At(GridCenter(10, 8));
  town_spawn.scale = kGridSize;
  town_spawn.collision = kGridSize;
  world->SpawnOnMap(EntityType::kTown, town_spawn);

  EntityPtr march = world->SpawnOnMap(EntityType::kMarch,
                                      EntitySpawn::At(GridCenter(8, 8)));
  world->Move().SetMoveSpeed(march, 200);
  bool done = false;
  world->Move().RequestMoveTo(
      march, GridCenter(12, 8),
      [&](const EntityPtr&, bool ok, MoveStopReason) {
        EXPECT_TRUE(ok);
        done = true;
      });
  for (int i = 0; i < 3000 && !done; ++i) {
    world->Tick();
  }
  EXPECT_TRUE(done);
}

GAME_TEST(RegressionTest, EntityComponentRegistryMove) {
  auto world = MakeWorld(32, 32);
  EntityPtr e = world->Spawn(EntityType::kMarch,
                             EntitySpawn::At(GridCenter(4, 4)));
  EXPECT_TRUE(e->HasComponent<MoveComponent>());
  EXPECT_NE(e->GetComponent<MoveComponent>(), nullptr);
  EXPECT_EQ(e->GetComponent<MoveComponent>(), e->Move());

  EXPECT_TRUE(e->RemoveComponent<MoveComponent>());
  EXPECT_FALSE(e->HasComponent<MoveComponent>());
  EXPECT_FALSE(world->Move().RequestMoveTo(e, GridCenter(6, 4), nullptr));

  e->AddComponent<MoveComponent>();
  world->EnterMap(e);
  EXPECT_TRUE(world->Move().RequestMoveTo(e, GridCenter(6, 4), nullptr));
}
