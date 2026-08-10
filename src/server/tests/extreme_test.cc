// 极端场景与规模压测；ScaleTest 用 GAME_SCALE_ENTITY_COUNT 控制数量（默认 800）。

#include <cstdint>
#include <memory>
#include <vector>

#include "ecs/entity/entity.h"
#include "common/aoi_def.h"
#include "ecs/components/move_component.h"
#include "ecs/systems/world_system.h"
#include "test_harness.h"
#include "test_map_invariants.h"

namespace {
using test::MakeDefaultEntityFactory;

Vector3D GridCenter(uint32_t gx, uint32_t gy, uint32_t gz = 0) {
  return Vector3D(MapGridIndexToCenterWorld(static_cast<int32_t>(gx)),
                  MapGridIndexToCenterWorld(static_cast<int32_t>(gy)),
                  MapGridIndexToCenterWorld(static_cast<int32_t>(gz)));
}

std::shared_ptr<WorldSystem> MakeWorld(uint32_t w, uint32_t h, uint32_t d = 8,
                                       MapGridStorage storage =
                                           MapGridStorage::kArray) {
  auto world = WorldSystem::Create(SceneRegionType::kMap);
  world->SetEntityFactory(MakeDefaultEntityFactory());
  world->Init();
  return world;
}

}  // namespace

GAME_TEST_SUITE(ExtremeTest);

GAME_TEST(ExtremeTest, SparseStorageAoiMoveLeave) {
  auto world = MakeWorld(128, 128, 8, MapGridStorage::kHash);
  EXPECT_TRUE(world->Map().GridStorage() == MapGridStorage::kHash);

  EntityPtr player =
      world->SpawnOnMap(EntityType::kPlayer,
                        EntitySpawn::At(GridCenter(30, 30)));
  EntityPtr march = world->SpawnOnMap(EntityType::kMarch,
                                      EntitySpawn::At(GridCenter(31, 30)));

  world->Move().SetMoveSpeed(march, 200);
  EXPECT_TRUE(world->Move().RequestMoveTo(march, GridCenter(35, 30), nullptr));
  for (int i = 0; i < 500; ++i) {
    world->Tick();
  }
  world->LeaveMap(march);
  world->LeaveMap(player);
  for (int i = 0; i < 50; ++i) {
    world->Tick();
  }
}

GAME_TEST(ExtremeTest, DoubleLeaveMapIsSafe) {
  auto world = MakeWorld(48, 48);
  EntityPtr e = world->SpawnOnMap(EntityType::kMarch,
                                  EntitySpawn::At(GridCenter(10, 10)));
  world->LeaveMap(e);
  world->LeaveMap(e);
  EXPECT_FALSE(e->IsInMap());
}

GAME_TEST(ExtremeTest, LeaveWhileMovingDoesNotCrash) {
  auto world = MakeWorld(64, 64);
  EntityPtr m = world->SpawnOnMap(EntityType::kMarch,
                                  EntitySpawn::At(GridCenter(8, 8)));
  world->Move().SetMoveSpeed(m, 100);
  EXPECT_TRUE(world->Move().RequestMoveTo(m, GridCenter(40, 8), nullptr));
  world->Tick();
  world->LeaveMap(m);
  world->Tick();
  world->Tick();
}

GAME_TEST(ExtremeTest, MapCornerSpawnAndWatcher) {
  auto world = MakeWorld(64, 64);
  EntityPtr player =
      world->SpawnOnMap(EntityType::kPlayer,
                        EntitySpawn::At(GridCenter(1, 1)));
  EntityPtr npc = world->SpawnOnMap(EntityType::kMarch,
                                    EntitySpawn::At(GridCenter(2, 1)));
  const uint32_t far_gx = 58;
  const uint32_t far_gy = 58;
  EntityPtr far_npc = world->SpawnOnMap(EntityType::kMarch,
                                        EntitySpawn::At(GridCenter(far_gx, far_gy)));
  world->MoveEntity(player, GridCenter(far_gx - 1, far_gy));
  (void)npc;
  (void)far_npc;
  world->Tick();
}

GAME_TEST(ExtremeTest, ReplaceMoveComponentOnMap) {
  auto world = MakeWorld(32, 32);
  EntityPtr e = world->SpawnOnMap(EntityType::kMarch,
                                  EntitySpawn::At(GridCenter(5, 5)));
  EXPECT_TRUE(e->RemoveComponent<MoveComponent>());
  e->AddComponent<MoveComponent>();
  world->Move().SetMoveSpeed(e, 80);
  EXPECT_TRUE(world->Move().RequestMoveTo(e, GridCenter(8, 5), nullptr));
  world->Tick();
}

GAME_TEST(ExtremeTest, EmptyWorldManyTicks) {
  auto world = MakeWorld(32, 32);
  for (int i = 0; i < 2000; ++i) {
    world->Tick();
  }
}

GAME_TEST(ExtremeTest, MaxConcurrentSpawnsNearCap) {
  auto world = MakeWorld(96, 96);
  std::vector<EntityPtr> batch;
  batch.reserve(400);
  for (int i = 0; i < 400; ++i) {
    const uint32_t gx = 4 + static_cast<uint32_t>(i % 40);
    const uint32_t gy = 4 + static_cast<uint32_t>(i / 40);
    batch.push_back(world->SpawnOnMap(EntityType::kMarch,
                                      EntitySpawn::At(GridCenter(gx, gy))));
  }
  for (const EntityPtr& e : batch) {
    world->LeaveMap(e);
  }
  world->Tick();
}

GAME_TEST(ExtremeTest, ScaledTownOnViewCellBoundary) {
  auto world = MakeWorld(128, 128);
  world->SpawnOnMap(EntityType::kPlayer,
                    EntitySpawn::At(GridCenter(7, 7)));
  EntitySpawn town_spawn = EntitySpawn::At(GridCenter(7, 7));
  town_spawn.scale = kGridSize;
  EntityPtr town =
      world->SpawnOnMap(EntityType::kTown, town_spawn);
  world->LeaveMap(town);
  world->Tick();
}

GAME_TEST_SUITE(ScaleTest);

// 可配置数量（默认 800）单位进图、Tick、全部离图
GAME_TEST(ScaleTest, TenKEntitiesSpawnMoveLeave) {
  const int kEntityCount = test::ScaleEntityCount();

  auto world = MakeWorld(200, 200, 8, MapGridStorage::kHash);
  EXPECT_TRUE(world->Map().GridStorage() == MapGridStorage::kHash);

  EntityPtr player =
      world->SpawnOnMap(EntityType::kPlayer,
                        EntitySpawn::At(GridCenter(100, 100)));

  std::vector<EntityPtr> units;
  units.reserve(static_cast<size_t>(kEntityCount));

  for (int i = 0; i < kEntityCount; ++i) {
    const uint32_t gx = 70 + static_cast<uint32_t>(i % 90);
    const uint32_t gy = 70 + static_cast<uint32_t>((i / 90) % 90);
    units.push_back(world->SpawnOnMap(EntityType::kMarch,
                                      EntitySpawn::At(GridCenter(gx, gy))));
  }
  EXPECT_EQ(units.size(), static_cast<size_t>(kEntityCount));

  world->Move().SetMoveSpeed(units.front(), 150);
  world->Move().RequestMoveTo(units.front(), GridCenter(72, 70), nullptr);

  for (int t = 0; t < 32; ++t) {
    world->Tick();
  }

  size_t on_map = 0;
  for (const EntityPtr& e : units) {
    if (e && e->IsInMap()) {
      ++on_map;
    }
  }
  EXPECT_EQ(on_map, static_cast<size_t>(kEntityCount));

  for (const EntityPtr& e : units) {
    if (e && e->IsInMap()) {
      world->LeaveMap(e);
    }
  }
  world->LeaveMap(player);

  for (int t = 0; t < 16; ++t) {
    world->Tick();
  }

  size_t still_on_map = 0;
  for (const EntityPtr& e : units) {
    if (e && e->IsInMap()) {
      ++still_on_map;
    }
  }
  EXPECT_EQ(still_on_map, 0u);
  EXPECT_FALSE(player->IsInMap());
}
