// 多层 AOI（近景/远景）与脏同步优化回归。
#include "test_harness.h"
#include "test_map_invariants.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "common/aoi_def.h"
#include "ecs/entity/entity.h"
#include "ecs/systems/aoi_sector.h"
#include "ecs/systems/world_system.h"

namespace {

using test::GridCenter;
using test::MakeWorld;

bool ContainsId(const std::vector<uint64_t>& ids, uint64_t id) {
  return std::find(ids.begin(), ids.end(), id) != ids.end();
}

}  // namespace

GAME_TEST_SUITE(AoiMultiLevelTest);

GAME_TEST(AoiMultiLevelTest, SectorCountMatchesDetailLevels) {
  auto world = MakeWorld();
  EXPECT_EQ(world->Aoi().SectorCount(), kDetailLevelCount);
  EXPECT_TRUE(kDetailLevelCount >= 1u);
  EXPECT_TRUE(world->Aoi().Sector(kAoiDetailNear) != nullptr);
  if (kDetailLevelCount > 1) {
    EXPECT_TRUE(world->Aoi().Sector(kAoiDetailFar) != nullptr);
    EXPECT_EQ(world->Aoi().Sector(kAoiDetailNear)->CellSizeGrids(),
              kAoiCellWorldSize);
    EXPECT_EQ(world->Aoi().Sector(kAoiDetailFar)->CellSizeGrids(),
              kAoiFarCellWorldSize);
  }
}

// 近景看不到、远景能看到：模拟小地图 vs 大地图视野差
GAME_TEST(AoiMultiLevelTest, FarWatcherSeesBeyondNearRange) {
  if (kDetailLevelCount < 2) {
    return;  // 单层构建时跳过（保持可编译）
  }
  auto world = MakeWorld();

  // 间距约 200m：近景格 10m×邻域±1 ≈ 30m 不可见；远景格 500m 可见
  auto a = world->SpawnOnMap(EntityType::kPlayer,
                             EntitySpawn::At(JPH::Vec3(0.f, 0.f, 0.f)));
  auto b = world->SpawnOnMap(EntityType::kPlayer,
                             EntitySpawn::At(JPH::Vec3(200.f, 0.f, 0.f)));
  EXPECT_TRUE(a && b);

  // 默认进图挂近景 watcher
  auto near_vis = world->GetVisibleEntities(a->GetId());
  EXPECT_FALSE(ContainsId(near_vis, b->GetId()));

  // 切到远景大地图
  world->Aoi().RemoveWatcher(a, false);
  EXPECT_TRUE(world->Aoi().AddWatcher(a, a->GetPosition(), kAoiDetailFar));
  auto far_vis = world->GetVisibleEntities(a->GetId());
  EXPECT_TRUE(ContainsId(far_vis, b->GetId()));

  world->LeaveMap(a);
  world->LeaveMap(b);
}

// 脏同步：仅有近景观察者时，远景层不应因 EnsureCell 膨胀空格
GAME_TEST(AoiMultiLevelTest, MarkDirtyDoesNotEnsureEmptyFarCells) {
  if (kDetailLevelCount < 2) {
    return;
  }
  auto world = MakeWorld();
  auto a = world->SpawnOnMap(EntityType::kPlayer,
                             EntitySpawn::At(GridCenter(8, 8)));
  auto b = world->SpawnOnMap(EntityType::kPlayer,
                             EntitySpawn::At(GridCenter(8, 8)));
  EXPECT_TRUE(a && b);

  AoiSector* far = world->Aoi().Sector(kAoiDetailFar);
  EXPECT_TRUE(far != nullptr);
  const size_t far_cells_before = far->AllocatedCellCount();

  // 仅近景互见；远景格可能因进图已有 subject 格，但 MarkDirty 不得 Ensure 新格
  a->SetPropertyDirty(EntityPropertyType::kMove, /*sync_immediately=*/true);
  world->Aoi().FlushDirty();

  EXPECT_EQ(far->AllocatedCellCount(), far_cells_before);

  world->LeaveMap(a);
  world->LeaveMap(b);
}

// 近景脏更新仍能推到同层观察者
GAME_TEST(AoiMultiLevelTest, NearDirtyStillReachesNearWatchers) {
  auto world = MakeWorld();
  auto a = world->SpawnOnMap(EntityType::kPlayer,
                             EntitySpawn::At(GridCenter(5, 5)));
  auto b = world->SpawnOnMap(EntityType::kPlayer,
                             EntitySpawn::At(GridCenter(5, 5)));
  EXPECT_TRUE(a && b);

  int updates_to_b = 0;
  const uint64_t aid = a->GetId();
  const uint64_t bid = b->GetId();
  world->Aoi().SetEntityUpdateCallback(
      [&](uint64_t viewer, uint64_t subject) {
        if (viewer == bid && subject == aid) {
          ++updates_to_b;
        }
      });

  updates_to_b = 0;
  a->SetPropertyDirty(EntityPropertyType::kMove, /*sync_immediately=*/true);
  EXPECT_GE(updates_to_b, 1);

  world->LeaveMap(a);
  world->LeaveMap(b);
}

// LeaveMap 家格命中后跳过全表扫：离图后近/远景格无残留 monitor
GAME_TEST(AoiMultiLevelTest, LeaveMapClearsAllDetailLevels) {
  auto world = MakeWorld();
  auto a = world->SpawnOnMap(EntityType::kPlayer,
                             EntitySpawn::At(GridCenter(3, 3)));
  auto b = world->SpawnOnMap(EntityType::kPlayer,
                             EntitySpawn::At(GridCenter(3, 3)));

  world->LeaveMap(a);

  for (size_t i = 0; i < world->Aoi().SectorCount(); ++i) {
    AoiSector* sec = world->Aoi().Sector(static_cast<int>(i));
    EXPECT_TRUE(sec != nullptr);
    if (AoiCell* cell = sec->CellAtPosition(a->GetPosition())) {
      EXPECT_FALSE(cell->Monitor().IsMonitoring(a->GetId()));
    }
  }
  // b 仍在图，可见列表不含 a
  auto vis = world->GetVisibleEntities(b->GetId());
  EXPECT_FALSE(ContainsId(vis, a->GetId()));

  world->LeaveMap(b);
  EXPECT_EQ(world->Map().AllocatedGridCount(), 0u);
}

// 近/远景同时有观察者时，脏更新必须两层都能收到（验证 FlushDirty 不清早清 PropertyTypes）
GAME_TEST(AoiMultiLevelTest, DirtyFlushReachesBothNearAndFarWatchers) {
  if (kDetailLevelCount < 2) {
    return;
  }
  auto world = MakeWorld();

  auto subject = world->SpawnOnMap(
      EntityType::kPlayer, EntitySpawn::At(JPH::Vec3(0.f, 0.f, 0.f)));
  auto near_w = world->SpawnOnMap(
      EntityType::kPlayer, EntitySpawn::At(JPH::Vec3(5.f, 0.f, 0.f)));
  auto far_w = world->SpawnOnMap(
      EntityType::kPlayer, EntitySpawn::At(JPH::Vec3(100.f, 0.f, 0.f)));
  EXPECT_TRUE(subject && near_w && far_w);

  // far_w 切到远景层，才能看见 subject（100m 超出近景邻域）
  world->Aoi().RemoveWatcher(far_w, false);
  EXPECT_TRUE(
      world->Aoi().AddWatcher(far_w, far_w->GetPosition(), kAoiDetailFar));

  int near_updates = 0;
  int far_updates = 0;
  const uint64_t sid = subject->GetId();
  const uint64_t nid = near_w->GetId();
  const uint64_t fid = far_w->GetId();
  world->Aoi().SetEntityUpdateCallback(
      [&](uint64_t viewer, uint64_t subj) {
        if (subj != sid) return;
        if (viewer == nid) ++near_updates;
        if (viewer == fid) ++far_updates;
      });

  subject->SetPropertyDirty(EntityPropertyType::kMove,
                            /*sync_immediately=*/false);
  world->Aoi().FlushDirty();

  EXPECT_GE(near_updates, 1);
  EXPECT_GE(far_updates, 1);

  world->LeaveMap(subject);
  world->LeaveMap(near_w);
  world->LeaveMap(far_w);
}

// 切换 detail_level：Detach 旧层 + Attach 新层，可见集随层变化
GAME_TEST(AoiMultiLevelTest, SwitchDetailLevelRebuildsVisibility) {
  if (kDetailLevelCount < 2) {
    return;
  }
  auto world = MakeWorld();
  auto a = world->SpawnOnMap(EntityType::kPlayer,
                             EntitySpawn::At(JPH::Vec3(0.f, 0.f, 0.f)));
  auto near_n = world->SpawnOnMap(
      EntityType::kPlayer, EntitySpawn::At(JPH::Vec3(5.f, 0.f, 0.f)));
  auto far_only = world->SpawnOnMap(
      EntityType::kPlayer, EntitySpawn::At(JPH::Vec3(200.f, 0.f, 0.f)));

  auto near_vis = world->GetVisibleEntities(a->GetId());
  EXPECT_TRUE(ContainsId(near_vis, near_n->GetId()));
  EXPECT_FALSE(ContainsId(near_vis, far_only->GetId()));

  world->Aoi().RemoveWatcher(a, false);
  EXPECT_TRUE(world->Aoi().AddWatcher(a, a->GetPosition(), kAoiDetailFar));
  auto far_vis = world->GetVisibleEntities(a->GetId());
  EXPECT_TRUE(ContainsId(far_vis, near_n->GetId()));
  EXPECT_TRUE(ContainsId(far_vis, far_only->GetId()));

  world->LeaveMap(a);
  world->LeaveMap(near_n);
  world->LeaveMap(far_only);
}
