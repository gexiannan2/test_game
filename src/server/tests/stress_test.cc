// stress_test.cc — 服务端高频进离图压力（更大规模见 aoi_mass / LoginAoiChurn）
#include "test_harness.h"
#include "test_map_invariants.h"
#include "test_world_invariants.h"

#include <vector>

using test::AssertWorldConsistent;
using test::GridCenter;
using test::MakeWorld;

GAME_TEST_SUITE(StressTest);

GAME_TEST(StressTest, BurstEnterLeaveSameCell) {
  auto world = MakeWorld();
  for (int round = 0; round < 20; ++round) {
    std::vector<EntityPtr> batch;
    for (int i = 0; i < 50; ++i) {
      batch.push_back(world->SpawnOnMap(
          EntityType::kPlayer, EntitySpawn::At(GridCenter(15, 15))));
    }
    for (auto& e : batch) world->LeaveMap(e);
  }
  EXPECT_EQ(world->Map().AllocatedGridCount(), 0u);
}

GAME_TEST(StressTest, AlternatingOnlineOfflineSameCohort) {
  auto world = MakeWorld();
  constexpr int kN = 80;
  std::vector<EntityPtr> cohort;
  cohort.reserve(kN);
  for (int i = 0; i < kN; ++i) {
    cohort.push_back(world->SpawnOnMap(
        EntityType::kPlayer, EntitySpawn::At(GridCenter(18, 18))));
  }
  AssertWorldConsistent(*world, cohort);

  for (int wave = 0; wave < 30; ++wave) {
    for (int i = 0; i < kN; ++i) {
      if ((i + wave) % 2 == 0) {
        if (cohort[i]->IsInMap()) {
          world->LeaveMap(cohort[i]);
        }
      } else if (!cohort[i]->IsInMap()) {
        world->EnterMap(cohort[i]);
      }
    }
    AssertWorldConsistent(*world, cohort);
  }

  for (auto& e : cohort) {
    if (e->IsInMap()) {
      world->LeaveMap(e);
    }
  }
  EXPECT_EQ(world->Map().AllocatedGridCount(), 0u);
}
