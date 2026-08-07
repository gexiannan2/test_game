// AOI 可见性与独立 Oracle 对拍，覆盖移动、进出图、随机生命周期。

#include <cstdint>
#include <memory>
#include <vector>

#include "test_aoi_oracle.h"
#include "test_bind_callbacks.h"
#include "test_harness.h"

using test::aoi_oracle::AssertModelMatchesOracle;
using test::aoi_oracle::GridCenter;
using test::aoi_oracle::MakeWorld;
using test::aoi_oracle::Rng;
using test::aoi_oracle::SpawnOnGrid;
using test::aoi_oracle::VisibilityModel;

namespace {

// 随机种子 + 多步操作（移动/刷怪/离图/标脏），每步 AssertModelMatchesOracle
void RunRandomizedLifecycleScenario(uint64_t seed, int steps) {
  auto world = MakeWorld(96, 96);
  auto model = std::make_shared<VisibilityModel>();
  test::BindAoiCallbacks(*world, model);

  const AoiSector* load = world->Aoi().Sector(0);
  const int32_t max_cx = load->MaxCellX();
  const int32_t max_cy = load->MaxCellY();
  const int32_t max_cz = load->MaxCellZ();

  std::vector<EntityPtr> players;
  std::vector<EntityPtr> subjects;
  Rng rng(seed);

  for (int i = 0; i < 6; ++i) {
    players.push_back(SpawnOnGrid(world, 8 + static_cast<uint32_t>(i * 5), 12,
                                  EntityType::kPlayer));
  }
  for (int i = 0; i < 24; ++i) {
    subjects.push_back(SpawnOnGrid(world, 4 + rng.Range(40), 4 + rng.Range(40)));
  }

  auto all_subjects = [&]() {
    std::vector<EntityPtr> all = subjects;
    for (const EntityPtr& p : players) {
      all.push_back(p);
    }
    return all;
  };

  AssertModelMatchesOracle(*world, *model, players, all_subjects(), max_cx,
                           max_cy, max_cz, "random_initial");

  for (int step = 0; step < steps; ++step) {
    const int op = static_cast<int>(rng.Range(5));
    switch (op) {
      case 0: {
        if (!subjects.empty()) {
          EntityPtr& e = subjects[rng.Range(static_cast<uint32_t>(subjects.size()))];
          if (e && e->IsInMap()) {
            world->MoveEntity(
                e, GridCenter(2 + rng.Range(44), 2 + rng.Range(44)),
                EntityPropertyType::kMove);
          }
        }
        break;
      }
      case 1: {
        if (!players.empty()) {
          EntityPtr& p = players[rng.Range(static_cast<uint32_t>(players.size()))];
          if (p && p->IsInMap()) {
            world->MoveEntity(
                p, GridCenter(2 + rng.Range(44), 2 + rng.Range(44)),
                EntityPropertyType::kMove);
          }
        }
        break;
      }
      case 2: {
        subjects.push_back(
            SpawnOnGrid(world, 2 + rng.Range(44), 2 + rng.Range(44)));
        break;
      }
      case 3: {
        if (!subjects.empty()) {
          size_t idx = rng.Range(static_cast<uint32_t>(subjects.size()));
          EntityPtr e = subjects[idx];
          if (e && e->IsInMap()) {
            world->LeaveMap(e);
          }
          subjects.erase(subjects.begin() + static_cast<ptrdiff_t>(idx));
        }
        break;
      }
      case 4: {
        if (!subjects.empty()) {
          EntityPtr& e = subjects[rng.Range(static_cast<uint32_t>(subjects.size()))];
          if (e && e->IsInMap()) {
            e->SetPropertyDirty(EntityPropertyType::kHomeObject);
            world->Tick();
          }
        }
        break;
      }
      default:
        break;
    }
    AssertModelMatchesOracle(*world, *model, players, all_subjects(), max_cx,
                             max_cy, max_cz, "random_step");
  }
}

}  // namespace

GAME_TEST_SUITE(AoiParityTest);

GAME_TEST(AoiParityTest, SpawnGridSweepMatchesOracle) {
  auto world = MakeWorld(128, 128);
  auto model = std::make_shared<VisibilityModel>();
  test::BindAoiCallbacks(*world, model);

  const AoiSector* load = world->Aoi().Sector(0);
  EXPECT_NE(load, nullptr);
  const int32_t max_cx = load->MaxCellX();
  const int32_t max_cy = load->MaxCellY();
  const int32_t max_cz = load->MaxCellZ();

  std::vector<EntityPtr> players;
  std::vector<EntityPtr> subjects;

  EntityPtr player = SpawnOnGrid(world, 12, 12, EntityType::kPlayer);
  players.push_back(player);

  for (uint32_t ngx = 4; ngx < 36; ngx += 2) {
    for (uint32_t ngy = 4; ngy < 36; ngy += 2) {
      EntityPtr npc = SpawnOnGrid(world, ngx, ngy);
      subjects.push_back(npc);
      AssertModelMatchesOracle(*world, *model, players, subjects, max_cx,
                               max_cy, max_cz, "spawn_sweep");
    }
  }
}

GAME_TEST(AoiParityTest, SubjectCrossesViewCellBoundary) {
  auto world = MakeWorld(128, 128);
  auto model = std::make_shared<VisibilityModel>();
  test::BindAoiCallbacks(*world, model);

  const AoiSector* load = world->Aoi().Sector(0);
  const int32_t max_cx = load->MaxCellX();
  const int32_t max_cy = load->MaxCellY();
  const int32_t max_cz = load->MaxCellZ();

  EntityPtr player = SpawnOnGrid(world, 10, 10, EntityType::kPlayer);
  EntityPtr npc = SpawnOnGrid(world, 15, 10);
  std::vector<EntityPtr> players = {player};
  std::vector<EntityPtr> subjects = {npc};

  AssertModelMatchesOracle(*world, *model, players, subjects, max_cx, max_cy, max_cz,
                           "before_move");

  world->MoveEntity(npc, GridCenter(40, 40), EntityPropertyType::kMove);
  AssertModelMatchesOracle(*world, *model, players, subjects, max_cx, max_cy, max_cz,
                           "after_far_move");

  world->MoveEntity(npc, GridCenter(11, 10), EntityPropertyType::kMove);
  AssertModelMatchesOracle(*world, *model, players, subjects, max_cx, max_cy, max_cz,
                           "after_return");
}

GAME_TEST(AoiParityTest, WatcherMoveChangesVisibility) {
  auto world = MakeWorld(128, 128);
  auto model = std::make_shared<VisibilityModel>();
  test::BindAoiCallbacks(*world, model);

  const AoiSector* load = world->Aoi().Sector(0);
  const int32_t max_cx = load->MaxCellX();
  const int32_t max_cy = load->MaxCellY();
  const int32_t max_cz = load->MaxCellZ();

  EntityPtr player = SpawnOnGrid(world, 6, 6, EntityType::kPlayer);
  EntityPtr far_npc = SpawnOnGrid(world, 50, 50);
  std::vector<EntityPtr> players = {player};
  std::vector<EntityPtr> subjects = {far_npc};

  AssertModelMatchesOracle(*world, *model, players, subjects, max_cx, max_cy, max_cz,
                           "watcher_at_origin");

  world->MoveEntity(player, GridCenter(48, 48), EntityPropertyType::kMove);
  AssertModelMatchesOracle(*world, *model, players, subjects, max_cx, max_cy, max_cz,
                           "watcher_near_far_npc");
}

GAME_TEST(AoiParityTest, TwoPlayersMoveApartMatchesOracle) {
  auto world = MakeWorld(128, 128);
  auto model = std::make_shared<VisibilityModel>();
  test::BindAoiCallbacks(*world, model);

  const AoiSector* load = world->Aoi().Sector(0);
  const int32_t max_cx = load->MaxCellX();
  const int32_t max_cy = load->MaxCellY();
  const int32_t max_cz = load->MaxCellZ();

  EntityPtr p1 = SpawnOnGrid(world, 14, 14, EntityType::kPlayer);
  EntityPtr p2 = SpawnOnGrid(world, 15, 14, EntityType::kPlayer);
  std::vector<EntityPtr> players = {p1, p2};
  std::vector<EntityPtr> subjects = {p1, p2};

  world->MoveEntity(p2, GridCenter(60, 60), EntityPropertyType::kMove);
  AssertModelMatchesOracle(*world, *model, players, subjects, max_cx, max_cy, max_cz,
                           "two_players_apart");
}

GAME_TEST(AoiParityTest, RandomizedLifecycleSequence) {
  RunRandomizedLifecycleScenario(0xA01Cafe, 180);
}

GAME_TEST(AoiParityTest, RandomizedLifecycleMultipleSeeds) {
  const uint64_t seeds[] = {1u,       42u,       0xDEADBEEFu,
                            0x55AA55AAu, 0x12345678u, 0xC0FFEEu};
  for (uint64_t seed : seeds) {
    RunRandomizedLifecycleScenario(seed, 120);
  }
}

GAME_TEST(AoiParityTest, NestedPlayerNpcEnterLeave) {
  auto world = MakeWorld(64, 64);
  auto model = std::make_shared<VisibilityModel>();
  test::BindAoiCallbacks(*world, model);

  const AoiSector* load = world->Aoi().Sector(0);
  const int32_t max_cx = load->MaxCellX();
  const int32_t max_cy = load->MaxCellY();
  const int32_t max_cz = load->MaxCellZ();

  std::vector<EntityPtr> players;
  std::vector<EntityPtr> subjects;

  for (int round = 0; round < 3; ++round) {
  for (uint32_t i = 0; i < 4; ++i) {
    EntityPtr p = SpawnOnGrid(world, 6 + i * 2, 6 + round, EntityType::kPlayer);
    players.push_back(p);
    for (uint32_t j = 0; j < 3; ++j) {
      EntityPtr n = SpawnOnGrid(world, 7 + i * 2 + j, 7 + round);
      subjects.push_back(n);
      std::vector<EntityPtr> all = subjects;
      for (const EntityPtr& pl : players) {
        all.push_back(pl);
      }
      AssertModelMatchesOracle(*world, *model, players, all, max_cx, max_cy, max_cz,
                               "nested_enter");
    }
  }
  while (!subjects.empty()) {
    EntityPtr n = subjects.back();
    subjects.pop_back();
    if (n && n->IsInMap()) {
      world->LeaveMap(n);
    }
    std::vector<EntityPtr> all = subjects;
    for (const EntityPtr& pl : players) {
      all.push_back(pl);
    }
    AssertModelMatchesOracle(*world, *model, players, all, max_cx, max_cy, max_cz,
                             "nested_npc_leave");
  }
  while (!players.empty()) {
    EntityPtr p = players.back();
    players.pop_back();
    if (p && p->IsInMap()) {
      world->LeaveMap(p);
    }
    std::vector<EntityPtr> all = subjects;
    for (const EntityPtr& pl : players) {
      all.push_back(pl);
    }
    AssertModelMatchesOracle(*world, *model, players, all, max_cx, max_cy, max_cz,
                             "nested_player_leave");
  }
  }
}

GAME_TEST(AoiParityTest, FootprintCrossingViewCellLeaveNoStale) {
  auto world = MakeWorld(128, 128);
  auto model = std::make_shared<VisibilityModel>();
  test::BindAoiCallbacks(*world, model);

  const AoiSector* load = world->Aoi().Sector(0);
  const int32_t max_cx = load->MaxCellX();
  const int32_t max_cy = load->MaxCellY();
  const int32_t max_cz = load->MaxCellZ();

  EntityPtr player = SpawnOnGrid(world, 7, 7, EntityType::kPlayer);
  EntityPtr unit = SpawnOnGrid(world, 7, 7);
  std::vector<EntityPtr> players = {player};
  std::vector<EntityPtr> subjects = {unit};

  world->LeaveMap(unit);
  subjects.clear();
  AssertModelMatchesOracle(*world, *model, players, subjects, max_cx, max_cy, max_cz,
                           "footprint_leave");
}

GAME_TEST(AoiParityTest, WatcherMoveLosesFarSubject) {
  auto world = MakeWorld(128, 128);
  auto model = std::make_shared<VisibilityModel>();
  test::BindAoiCallbacks(*world, model);

  const AoiSector* load = world->Aoi().Sector(0);
  const int32_t max_cx = load->MaxCellX();
  const int32_t max_cy = load->MaxCellY();
  const int32_t max_cz = load->MaxCellZ();

  EntityPtr player = SpawnOnGrid(world, 10, 10, EntityType::kPlayer);
  EntityPtr npc = SpawnOnGrid(world, 11, 10);
  std::vector<EntityPtr> players = {player};
  std::vector<EntityPtr> subjects = {npc};

  world->MoveEntity(player, GridCenter(55, 55), EntityPropertyType::kMove);
  AssertModelMatchesOracle(*world, *model, players, subjects, max_cx, max_cy, max_cz,
                           "watcher_move_away");
}

GAME_TEST(AoiParityTest, WatcherMicroMoveKeepsVisibility) {
  auto world = MakeWorld(64, 64);
  auto model = std::make_shared<VisibilityModel>();
  test::BindAoiCallbacks(*world, model);

  const AoiSector* load = world->Aoi().Sector(0);
  const int32_t max_cx = load->MaxCellX();
  const int32_t max_cy = load->MaxCellY();
  const int32_t max_cz = load->MaxCellZ();

  EntityPtr player = SpawnOnGrid(world, 20, 20, EntityType::kPlayer);
  EntityPtr npc = SpawnOnGrid(world, 21, 20);
  std::vector<EntityPtr> players = {player};
  std::vector<EntityPtr> subjects = {npc};

  world->MoveEntity(player, GridCenter(20, 21), EntityPropertyType::kMove);
  AssertModelMatchesOracle(*world, *model, players, subjects, max_cx, max_cy, max_cz,
                           "watcher_micro_move");
}
