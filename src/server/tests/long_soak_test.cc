// long_soak_test.cc — 浸泡：默认短跑；GAME_SOAK_SECONDS / GAME_SOAK_UNLIMITED=1 加长
#include <cstdlib>
#include <memory>
#include <vector>

#include "test_aoi_oracle.h"
#include "test_bind_callbacks.h"
#include "test_harness.h"
#include "test_map_invariants.h"

using test::GridCenter;
using test::MakeWorld;
using test::aoi_oracle::Rng;

GAME_TEST_SUITE(LongSoakTest);

namespace {
struct CountingSceneSync {
  void OnEntityEnter(uint64_t, const std::vector<uint64_t>&) {}
  void OnEntityLeave(uint64_t, const std::vector<uint64_t>&) {}
  void OnEntityUpdate(uint64_t, uint64_t) {}
};

int SoakSeconds() {
  if (const char* u = std::getenv("GAME_SOAK_UNLIMITED")) {
    if (u[0] == '1') return 3600;
  }
  if (const char* s = std::getenv("GAME_SOAK_SECONDS")) {
    const int n = std::atoi(s);
    if (n > 0) return n;
  }
  return 2;
}
}  // namespace

GAME_TEST(LongSoakTest, RecursiveRandomAoiMoveChurn) {
  auto world = MakeWorld();
  auto sync = std::make_shared<CountingSceneSync>();
  test::BindAoiCallbacks(*world, sync);
  Rng rng(20260801);
  std::vector<EntityPtr> players;
  for (int i = 0; i < 32; ++i) {
    players.push_back(world->SpawnOnMap(
        EntityType::kPlayer,
        EntitySpawn::At(GridCenter(30 + (i % 5), 30 + (i / 5)))));
  }
  const int ticks = SoakSeconds() * 60;
  for (int t = 0; t < ticks; ++t) {
    for (size_t i = 0; i < players.size(); ++i) {
      if (!players[i] || !players[i]->IsInMap()) continue;
      if (rng.Range(20) == 0) {
        world->LeaveMap(players[i]);
        players[i] = world->SpawnOnMap(
            EntityType::kPlayer,
            EntitySpawn::At(GridCenter(30 + static_cast<int>(rng.Range(8)),
                                       30 + static_cast<int>(rng.Range(8)))));
      } else if (rng.Range(3) == 0) {
        world->MoveEntity(
            players[i],
            GridCenter(25 + static_cast<int>(rng.Range(15)),
                       25 + static_cast<int>(rng.Range(15))));
      }
    }
    world->Tick();
  }
  for (auto& p : players) {
    if (p && p->IsInMap()) world->LeaveMap(p);
  }
  EXPECT_TRUE(true);
}

GAME_TEST(LongSoakTest, ContinuousTickSoak) {
  auto world = MakeWorld();
  EntityPtr a = world->SpawnOnMap(EntityType::kPlayer,
                                  EntitySpawn::At(GridCenter(8, 8)));
  EntityPtr b = world->SpawnOnMap(EntityType::kPlayer,
                                  EntitySpawn::At(GridCenter(9, 8)));
  const int ticks = SoakSeconds() * 60;
  for (int i = 0; i < ticks; ++i) {
    world->MoveEntity(a, GridCenter(8 + (i % 3), 8));
    world->MoveEntity(b, GridCenter(9 - (i % 3), 8));
    world->Tick();
  }
  world->LeaveMap(a);
  world->LeaveMap(b);
  EXPECT_TRUE(true);
}
