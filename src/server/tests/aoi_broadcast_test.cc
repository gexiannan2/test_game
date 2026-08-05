// AOI 广播增量与 Oracle 对拍（enter/leave 条数与可见边差分一致）。
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <vector>

#include "test_aoi_oracle.h"
#include "test_bind_callbacks.h"
#include "test_harness.h"

namespace {

using test::aoi_oracle::AllOnMapSubjects;
using test::aoi_oracle::AssertBroadcastDeltaMatchesOracle;
using test::aoi_oracle::AssertModelMatchesOracle;
using test::aoi_oracle::BroadcastAuditSync;
using test::aoi_oracle::ComputeOracleVisiblePairs;
using test::aoi_oracle::DiffVisiblePairs;
using test::aoi_oracle::GridCenter;
using test::aoi_oracle::MakeWorld;
using test::aoi_oracle::Rng;
using test::aoi_oracle::SpawnOnGrid;
using test::aoi_oracle::VisPair;

#ifndef NDEBUG
constexpr int kMassPlayerCount = 96;
constexpr int kMassNpcBatch = 40;
constexpr int kReenterRounds = 120;
constexpr int kRandomSteps = 80;
#else
constexpr int kMassPlayerCount = 200;
constexpr int kMassNpcBatch = 60;
constexpr int kReenterRounds = 200;
constexpr int kRandomSteps = 150;
#endif

void RunStepWithBroadcastAudit(
    const std::shared_ptr<WorldSystem>& world, BroadcastAuditSync& sync,
    const std::vector<EntityPtr>& players, std::vector<EntityPtr>& npcs,
    int32_t max_cx, int32_t max_cy, int32_t max_cz, const std::set<VisPair>& pairs_before,
    const char* context, const std::function<void()>& action) {
  std::set<uint64_t> players_on_map_before;
  for (const EntityPtr& p : players) {
    if (p && p->IsInMap()) {
      players_on_map_before.insert(p->GetId());
    }
  }
  sync.ResetCounters();
  action();
  sync.SyncWatchersWhoLeftMap(players, players_on_map_before);
  world->Tick();

  const std::vector<EntityPtr> subjects = AllOnMapSubjects(players, npcs);
  const std::set<VisPair> pairs_after =
      ComputeOracleVisiblePairs(*world, players, subjects, max_cx, max_cy, max_cz);
  std::set<VisPair> added;
  std::set<VisPair> removed;
  DiffVisiblePairs(pairs_before, pairs_after, added, removed);

  AssertBroadcastDeltaMatchesOracle(sync.enter_subject_refs,
                                    sync.leave_subject_refs, added, removed,
                                    *world, context);
  AssertModelMatchesOracle(*world, sync.model, players, subjects, max_cx, max_cy, max_cz,
                           context);
}

}  // namespace

GAME_TEST_SUITE(AoiBroadcastTest);

GAME_TEST(AoiBroadcastTest, MassPlayersNpcEnterBroadcastCounts) {
  auto world = MakeWorld(160, 160);
  auto sync = std::make_shared<BroadcastAuditSync>();
  test::BindAoiCallbacks(*world, sync);

  const AoiSector* load = world->Aoi().Sector(0);
  EXPECT_NE(load, nullptr);
  const int32_t max_cx = load->MaxCellX();
  const int32_t max_cy = load->MaxCellY();
  const int32_t max_cz = load->MaxCellZ();

  std::vector<EntityPtr> players;
  players.reserve(static_cast<size_t>(kMassPlayerCount));
  constexpr uint32_t kCols = 20;
  for (int i = 0; i < kMassPlayerCount; ++i) {
    const uint32_t gx = 24 + static_cast<uint32_t>(i % kCols);
    const uint32_t gy = 24 + static_cast<uint32_t>(i / kCols);
    players.push_back(
        SpawnOnGrid(world, gx, gy, EntityType::kPlayer));
  }

  std::vector<EntityPtr> npcs;
  std::vector<EntityPtr> subjects = AllOnMapSubjects(players, npcs);
  std::set<VisPair> pairs =
      ComputeOracleVisiblePairs(*world, players, subjects, max_cx, max_cy, max_cz);
  AssertModelMatchesOracle(*world, sync->model, players, subjects, max_cx, max_cy, max_cz,
                           "mass_initial");

  for (int i = 0; i < kMassNpcBatch; ++i) {
    const uint32_t gx = 26 + static_cast<uint32_t>(i % 10);
    const uint32_t gy = 26 + static_cast<uint32_t>(i / 10);
    const std::set<VisPair> before = pairs;
    RunStepWithBroadcastAudit(
        world, *sync, players, npcs, max_cx, max_cy, max_cz, before,
        "mass_npc_enter",
        [&]() { npcs.push_back(SpawnOnGrid(world, gx, gy)); });
    pairs = ComputeOracleVisiblePairs(
        *world, players, AllOnMapSubjects(players, npcs), max_cx, max_cy, max_cz);
  }
}

GAME_TEST(AoiBroadcastTest, RepeatedPlayerEnterLeaveBroadcastCounts) {
  auto world = MakeWorld(128, 128);
  auto sync = std::make_shared<BroadcastAuditSync>();
  test::BindAoiCallbacks(*world, sync);

  const AoiSector* load = world->Aoi().Sector(0);
  const int32_t max_cx = load->MaxCellX();
  const int32_t max_cy = load->MaxCellY();
  const int32_t max_cz = load->MaxCellZ();

  std::vector<EntityPtr> players;
  for (int i = 0; i < 32; ++i) {
    players.push_back(SpawnOnGrid(world, 30 + static_cast<uint32_t>(i % 8),
                                  30 + static_cast<uint32_t>(i / 8),
                                  EntityType::kPlayer));
  }
  std::vector<EntityPtr> npcs;
  for (int i = 0; i < 24; ++i) {
    npcs.push_back(SpawnOnGrid(world, 31 + static_cast<uint32_t>(i % 6),
                               31 + static_cast<uint32_t>(i / 6)));
  }

  std::vector<EntityPtr> subjects = AllOnMapSubjects(players, npcs);
  std::set<VisPair> pairs =
      ComputeOracleVisiblePairs(*world, players, subjects, max_cx, max_cy, max_cz);

  Rng rng(0xE117A0u);
  for (int round = 0; round < kReenterRounds; ++round) {
    EntityPtr& p = players[rng.Range(static_cast<uint32_t>(players.size()))];
    if (!p) {
      continue;
    }

    if (p->IsInMap()) {
      const std::set<VisPair> before = pairs;
      RunStepWithBroadcastAudit(
          world, *sync, players, npcs, max_cx, max_cy, max_cz, before,
          "player_leave",
          [&]() { world->LeaveMap(p); });
      subjects = AllOnMapSubjects(players, npcs);
      pairs = ComputeOracleVisiblePairs(*world, players, subjects, max_cx,
                                        max_cy, max_cz);
    } else {
      const std::set<VisPair> before = pairs;
      RunStepWithBroadcastAudit(
          world, *sync, players, npcs, max_cx, max_cy, max_cz, before,
          "player_enter",
          [&]() {
            sync->SyncWatcherBeforeReenter(p->GetId(), p->IsInMap());
            world->EnterMap(p);
          });
      subjects = AllOnMapSubjects(players, npcs);
      pairs = ComputeOracleVisiblePairs(*world, players, subjects, max_cx,
                                        max_cy, max_cz);
    }
  }
}

GAME_TEST(AoiBroadcastTest, MassPlayersRandomChurnBroadcastCounts) {
  auto world = MakeWorld(192, 192);
  auto sync = std::make_shared<BroadcastAuditSync>();
  test::BindAoiCallbacks(*world, sync);

  const AoiSector* load = world->Aoi().Sector(0);
  const int32_t max_cx = load->MaxCellX();
  const int32_t max_cy = load->MaxCellY();
  const int32_t max_cz = load->MaxCellZ();

  std::vector<EntityPtr> players;
  players.reserve(static_cast<size_t>(kMassPlayerCount));
  for (int i = 0; i < kMassPlayerCount; ++i) {
    const uint32_t gx = 20 + static_cast<uint32_t>(i % 25);
    const uint32_t gy = 20 + static_cast<uint32_t>(i / 25);
    players.push_back(
        SpawnOnGrid(world, gx, gy, EntityType::kPlayer));
  }

  std::vector<EntityPtr> npcs;
  for (int i = 0; i < 36; ++i) {
    npcs.push_back(SpawnOnGrid(world, 22 + static_cast<uint32_t>(i % 8),
                               22 + static_cast<uint32_t>(i / 8)));
  }

  std::vector<EntityPtr> subjects = AllOnMapSubjects(players, npcs);
  std::set<VisPair> pairs =
      ComputeOracleVisiblePairs(*world, players, subjects, max_cx, max_cy, max_cz);
  Rng rng(0xB0ADCA57u);

  for (int step = 0; step < kRandomSteps; ++step) {
    const int op = static_cast<int>(rng.Range(6));
    const std::set<VisPair> before = pairs;
    const char* tag = "churn";

    RunStepWithBroadcastAudit(
        world, *sync, players, npcs, max_cx, max_cy, max_cz, before, tag, [&]() {
          switch (op) {
            case 0: {
              npcs.push_back(SpawnOnGrid(
                  world, 21 + rng.Range(14), 21 + rng.Range(14)));
              break;
            }
            case 1: {
              if (!npcs.empty()) {
                size_t idx = rng.Range(static_cast<uint32_t>(npcs.size()));
                EntityPtr e = npcs[idx];
                if (e && e->IsInMap()) {
                  world->LeaveMap(e);
                }
                npcs.erase(npcs.begin() + static_cast<ptrdiff_t>(idx));
              }
              break;
            }
            case 2: {
              if (!npcs.empty()) {
                EntityPtr& e =
                    npcs[rng.Range(static_cast<uint32_t>(npcs.size()))];
                if (e && e->IsInMap()) {
                  world->MoveEntity(
                      e, GridCenter(21 + rng.Range(14), 21 + rng.Range(14)));
                }
              }
              break;
            }
            case 3: {
              EntityPtr& p =
                  players[rng.Range(static_cast<uint32_t>(players.size()))];
              if (p && p->IsInMap()) {
                world->MoveEntity(
                    p, GridCenter(20 + rng.Range(16), 20 + rng.Range(16)));
              }
              break;
            }
            case 4: {
              EntityPtr& p =
                  players[rng.Range(static_cast<uint32_t>(players.size()))];
              if (p && p->IsInMap()) {
                world->LeaveMap(p);
              }
              break;
            }
            case 5: {
              EntityPtr& p =
                  players[rng.Range(static_cast<uint32_t>(players.size()))];
              if (p && !p->IsInMap()) {
                sync->SyncWatcherBeforeReenter(p->GetId(), false);
                world->EnterMap(p);
              }
              break;
            }
            default:
              break;
          }
        });

    subjects = AllOnMapSubjects(players, npcs);
    pairs = ComputeOracleVisiblePairs(*world, players, subjects, max_cx, max_cy, max_cz);
  }
}

GAME_TEST(AoiBroadcastTest, SingleWatcherNpcReenterBroadcastCounts) {
  auto world = MakeWorld(64, 64);
  auto sync = std::make_shared<BroadcastAuditSync>();
  test::BindAoiCallbacks(*world, sync);

  const AoiSector* load = world->Aoi().Sector(0);
  const int32_t max_cx = load->MaxCellX();
  const int32_t max_cy = load->MaxCellY();
  const int32_t max_cz = load->MaxCellZ();

  std::vector<EntityPtr> players = {
      SpawnOnGrid(world, 10, 10, EntityType::kPlayer)};
  std::vector<EntityPtr> npcs;
  std::vector<EntityPtr> subjects = AllOnMapSubjects(players, npcs);
  std::set<VisPair> pairs =
      ComputeOracleVisiblePairs(*world, players, subjects, max_cx, max_cy, max_cz);

  for (int r = 0; r < 500; ++r) {
    EntityPtr npc = world->Spawn(EntityType::kMarch,
                                 EntitySpawn::At(GridCenter(11, 10)));
    {
      const std::set<VisPair> before = pairs;
      RunStepWithBroadcastAudit(
          world, *sync, players, npcs, max_cx, max_cy, max_cz, before, "npc_enter",
          [&]() {
            world->EnterMap(npc);
            npcs.push_back(npc);
          });
      subjects = AllOnMapSubjects(players, npcs);
      pairs = ComputeOracleVisiblePairs(*world, players, subjects, max_cx,
                                        max_cy, max_cz);
    }
    {
      const std::set<VisPair> before = pairs;
      RunStepWithBroadcastAudit(
          world, *sync, players, npcs, max_cx, max_cy, max_cz, before, "npc_leave",
          [&]() {
            world->LeaveMap(npc);
            npcs.pop_back();
          });
      subjects = AllOnMapSubjects(players, npcs);
      pairs = ComputeOracleVisiblePairs(*world, players, subjects, max_cx,
                                        max_cy, max_cz);
    }
  }
}
