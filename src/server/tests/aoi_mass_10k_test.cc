// 大规模玩家 AOI 对拍；规模由 GAME_AOI_PLAYER_COUNT 等环境变量控制。

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <vector>

#include "test_aoi_oracle.h"
#include "test_bind_callbacks.h"
#include "test_harness.h"

namespace {

using test::MassAoiChurnSteps;
using test::MassAoiPlayerCount;
using test::aoi_oracle::AssertBroadcastDeltaMatchesOracle;
using test::aoi_oracle::AssertModelMatchesOracle;
using test::aoi_oracle::AssertWatchersEnterNotifyForSubject;
using test::aoi_oracle::AssertWatchersLeaveNotifyForSubject;
using test::aoi_oracle::BroadcastAuditSync;
using test::aoi_oracle::ComputeOracleVisiblePairs;
using test::aoi_oracle::DiffVisiblePairs;
using test::aoi_oracle::GridCenter;
using test::aoi_oracle::MakeWorld;
using test::aoi_oracle::Rng;
using test::aoi_oracle::SpawnOnGrid;
using test::aoi_oracle::VisPair;

// 在格 69/70 簇状刷 kPlayers 名玩家，保证同 AOI 视野格内互为潜在观察者
std::vector<EntityPtr> SpawnMassPlayers(
    const std::shared_ptr<WorldSystem>& world, int count) {
  std::vector<EntityPtr> players;
  players.reserve(static_cast<size_t>(count));
  // 簇状出生：格 69/70 的格中心同属 AOI 视野格 28（10m），保证邻格玩家互为观察者
  constexpr uint32_t kBase = 69;
  constexpr uint32_t kSpan = 45;
  for (int i = 0; i < count; ++i) {
    const uint32_t gx = kBase + static_cast<uint32_t>(i % 2);
    const uint32_t gy =
        kBase + static_cast<uint32_t>((i / 2) % kSpan);
    players.push_back(
        SpawnOnGrid(world, gx, gy, EntityType::kPlayer));
  }
  return players;
}

// 单步操作前后对比 Oracle 可见边集，断言 enter/leave 广播条数与差分一致
void RunStepBroadcastDelta(
    const std::shared_ptr<WorldSystem>& world, BroadcastAuditSync& sync,
    const std::vector<EntityPtr>& players, int32_t max_cx, int32_t max_cy,
    int32_t max_cz,
    const std::set<VisPair>& pairs_before, const char* context,
    const std::function<void()>& action) {
  std::set<uint64_t> on_map_before;
  for (const EntityPtr& p : players) {
    if (p && p->IsInMap()) {
      on_map_before.insert(p->GetId());
    }
  }
  sync.ResetCounters();
  action();
  sync.SyncWatchersWhoLeftMap(players, on_map_before);
  world->Tick();

  const std::set<VisPair> pairs_after =
      ComputeOracleVisiblePairs(*world, players, players, max_cx, max_cy, max_cz);
  std::set<VisPair> added;
  std::set<VisPair> removed;
  DiffVisiblePairs(pairs_before, pairs_after, added, removed);
  AssertBroadcastDeltaMatchesOracle(sync.enter_subject_refs,
                                    sync.leave_subject_refs, added, removed,
                                    *world, context);
}

}  // namespace

GAME_TEST_SUITE(AoiMass10kTest);

GAME_TEST(AoiMass10kTest, OnePlayerLeavePerViewerReceiveCounts) {
  const int kPlayers = MassAoiPlayerCount();
  auto world = MakeWorld(220, 220);
  auto sync = std::make_shared<BroadcastAuditSync>();
  test::BindAoiCallbacks(*world, sync);

  const AoiSector* load = world->Aoi().Sector(0);
  EXPECT_NE(load, nullptr);
  const int32_t max_cx = load->MaxCellX();
  const int32_t max_cy = load->MaxCellY();
  const int32_t max_cz = load->MaxCellZ();

  std::vector<EntityPtr> players = SpawnMassPlayers(world, kPlayers);
  EXPECT_EQ(players.size(), static_cast<size_t>(kPlayers));

  AssertModelMatchesOracle(*world, sync->model, players, players, max_cx, max_cy, max_cz,
                           "mass10k_initial");

  EntityPtr victim = players.front();
  EXPECT_TRUE(victim && victim->IsInMap());

  std::set<VisPair> pairs =
      ComputeOracleVisiblePairs(*world, players, players, max_cx, max_cy, max_cz);
  const std::set<VisPair> before = pairs;

  sync->ResetCounters();
  world->LeaveMap(victim);
  world->Tick();

  std::set<VisPair> added;
  std::set<VisPair> removed;
  const std::set<VisPair> after =
      ComputeOracleVisiblePairs(*world, players, players, max_cx, max_cy, max_cz);
  DiffVisiblePairs(before, after, added, removed);
  AssertBroadcastDeltaMatchesOracle(sync->enter_subject_refs,
                                    sync->leave_subject_refs, added, removed,
                                    *world, "one_player_leave");

  AssertWatchersLeaveNotifyForSubject(*world, players, sync->per_viewer, victim,
                                      max_cx, max_cy, max_cz, 1u, "one_player_leave");
}

GAME_TEST(AoiMass10kTest, OnePlayerReenterPerViewerReceiveCounts) {
  const int kPlayers = MassAoiPlayerCount();
  auto world = MakeWorld(220, 220);
  auto sync = std::make_shared<BroadcastAuditSync>();
  test::BindAoiCallbacks(*world, sync);

  const AoiSector* load = world->Aoi().Sector(0);
  const int32_t max_cx = load->MaxCellX();
  const int32_t max_cy = load->MaxCellY();
  const int32_t max_cz = load->MaxCellZ();

  std::vector<EntityPtr> players = SpawnMassPlayers(world, kPlayers);
  EntityPtr victim = players.front();
  world->LeaveMap(victim);
  world->Tick();

  std::set<VisPair> pairs =
      ComputeOracleVisiblePairs(*world, players, players, max_cx, max_cy, max_cz);
  const std::set<VisPair> before = pairs;

  sync->ResetCounters();
  sync->SyncWatcherBeforeReenter(victim->GetId(), victim->IsInMap());
  world->EnterMap(victim);
  world->Tick();

  std::set<VisPair> added;
  std::set<VisPair> removed;
  const std::set<VisPair> after =
      ComputeOracleVisiblePairs(*world, players, players, max_cx, max_cy, max_cz);
  DiffVisiblePairs(before, after, added, removed);
  AssertBroadcastDeltaMatchesOracle(sync->enter_subject_refs,
                                    sync->leave_subject_refs, added, removed,
                                    *world, "one_player_reenter");

  AssertWatchersEnterNotifyForSubject(*world, players, sync->per_viewer, victim,
                                      max_cx, max_cy, max_cz, 1u, "one_player_reenter");
}

GAME_TEST(AoiMass10kTest, MassPlayersEnterLeaveMoveChurn) {
  const int kPlayers = MassAoiPlayerCount();
  const int kSteps = MassAoiChurnSteps();
  auto world = MakeWorld(220, 220);
  auto sync = std::make_shared<BroadcastAuditSync>();
  test::BindAoiCallbacks(*world, sync);

  const AoiSector* load = world->Aoi().Sector(0);
  const int32_t max_cx = load->MaxCellX();
  const int32_t max_cy = load->MaxCellY();
  const int32_t max_cz = load->MaxCellZ();

  std::vector<EntityPtr> players = SpawnMassPlayers(world, kPlayers);
  std::set<VisPair> pairs =
      ComputeOracleVisiblePairs(*world, players, players, max_cx, max_cy, max_cz);

  Rng rng(0x10A0A0A0u);
  for (int step = 0; step < kSteps; ++step) {
    const int op = static_cast<int>(rng.Range(5));
    const std::set<VisPair> before = pairs;
    const char* tag = "mass10k_churn";

    RunStepBroadcastDelta(
        world, *sync, players, max_cx, max_cy, max_cz, before, tag, [&]() {
          EntityPtr& p =
              players[rng.Range(static_cast<uint32_t>(players.size()))];
          if (!p) {
            return;
          }
          switch (op) {
            case 0:
              if (p->IsInMap()) {
                world->LeaveMap(p);
              }
              break;
            case 1:
              if (!p->IsInMap()) {
                sync->SyncWatcherBeforeReenter(p->GetId(), false);
                world->EnterMap(p);
              }
              break;
            case 2:
              if (p->IsInMap()) {
                world->MoveEntity(
                    p, GridCenter(70 + rng.Range(90), 70 + rng.Range(90)));
              }
              break;
            case 3:
              if (p->IsInMap()) {
                world->MoveEntity(
                    p, GridCenter(70 + rng.Range(90), 70 + rng.Range(90)));
              }
              break;
            case 4:
              if (!p->IsInMap()) {
                sync->SyncWatcherBeforeReenter(p->GetId(), false);
                world->EnterMap(p);
              } else {
                world->LeaveMap(p);
              }
              break;
            default:
              break;
          }
        });

    pairs = ComputeOracleVisiblePairs(*world, players, players, max_cx, max_cy, max_cz);
  }

  AssertModelMatchesOracle(*world, sync->model, players, players, max_cx, max_cy, max_cz,
                           "mass10k_churn_final");
}

GAME_TEST(AoiMass10kTest, RoundRobinLeaveReenterPerViewerStats) {
  const int kPlayers = MassAoiPlayerCount();
  int kRounds = test::EnvInt("MOVE3_AOI_ROUND_ROBIN_ROUNDS", 0);
  if (kRounds <= 0) {
#ifndef NDEBUG
    kRounds = kPlayers > 64 ? 64 : kPlayers;
#else
    kRounds = kPlayers > 2000 ? 12 : (kPlayers > 128 ? 128 : kPlayers);
#endif
  }
  if (kRounds > kPlayers) {
    kRounds = kPlayers;
  }

  auto world = MakeWorld(220, 220);
  auto sync = std::make_shared<BroadcastAuditSync>();
  test::BindAoiCallbacks(*world, sync);

  const AoiSector* load = world->Aoi().Sector(0);
  const int32_t max_cx = load->MaxCellX();
  const int32_t max_cy = load->MaxCellY();
  const int32_t max_cz = load->MaxCellZ();

  std::vector<EntityPtr> players = SpawnMassPlayers(world, kPlayers);
  std::set<VisPair> pairs =
      ComputeOracleVisiblePairs(*world, players, players, max_cx, max_cy, max_cz);

  for (int r = 0; r < kRounds; ++r) {
    EntityPtr victim = players[static_cast<size_t>(r)];
    if (!victim) {
      continue;
    }

    {
      const std::set<VisPair> before = pairs;
      RunStepBroadcastDelta(world, *sync, players, max_cx, max_cy, max_cz, before,
                            "round_robin_leave", [&]() {
                              if (victim->IsInMap()) {
                                world->LeaveMap(victim);
                              }
                            });
      if (victim->IsInMap()) {
        continue;
      }
      AssertWatchersLeaveNotifyForSubject(*world, players, sync->per_viewer,
                                          victim, max_cx, max_cy, max_cz, 1u,
                                          "round_robin_leave");
      pairs = ComputeOracleVisiblePairs(*world, players, players, max_cx,
                                        max_cy, max_cz);
    }

    {
      const std::set<VisPair> before = pairs;
      RunStepBroadcastDelta(world, *sync, players, max_cx, max_cy, max_cz, before,
                            "round_robin_enter", [&]() {
                              sync->SyncWatcherBeforeReenter(victim->GetId(),
                                                            victim->IsInMap());
                              world->EnterMap(victim);
                            });
      AssertWatchersEnterNotifyForSubject(*world, players, sync->per_viewer,
                                          victim, max_cx, max_cy, max_cz, 1u,
                                          "round_robin_enter");
      pairs = ComputeOracleVisiblePairs(*world, players, players, max_cx,
                                        max_cy, max_cz);
    }
  }
}

// 大规模同图：A 与 B 相邻，B 移出 10×10×10 视野后双方互收 disappear，移回后互收 appear。
// 默认 128 人；GAME_AOI_STRESS=1 + GAME_AOI_PLAYER_COUNT=5000 跑五千人背景。
// 协议侧 5000 人 E2E（svc_game_3d_protocol_mass_test）前请先：ulimit -n 65535。
GAME_TEST(AoiMass10kTest, TwoPlayersMoveOutOfViewAndBack) {
  const int kPlayers = MassAoiPlayerCount();
  if (kPlayers < 2) {
    return;
  }

  auto world = MakeWorld(220, 220);
  auto sync = std::make_shared<BroadcastAuditSync>();
  test::BindAoiCallbacks(*world, sync);

  std::vector<EntityPtr> players = SpawnMassPlayers(world, kPlayers);
  EntityPtr a = players[0];
  EntityPtr b = players[1];
  EXPECT_TRUE(a && b && a->IsInMap() && b->IsInMap());

  const Vector3D anchor = GridCenter(50, 50);
  const Vector3D near_b =
      anchor + JPH::Vec3(2.f, 0.f, 2.f);  // 与 A 同 AOI 视野格
  world->MoveEntity(a, anchor);
  world->MoveEntity(b, near_b);
  world->Tick();

  sync->ResetCounters();
  // kAoiRadius=1 → 3x3 格, 需跨 2 格以上才能超出视野半径
  const Vector3D far_b =
      anchor +
      JPH::Vec3(static_cast<float>(kAoiCellWorldSize) * 2.f + 2.f, 0.f, 0.f);
  world->MoveEntity(b, far_b);
  world->Tick();

  EXPECT_GE(sync->per_viewer.LeaveStepCount(a->GetId(), b->GetId()), 1u);
  EXPECT_GE(sync->per_viewer.LeaveStepCount(b->GetId(), a->GetId()), 1u);

  sync->ResetCounters();
  world->MoveEntity(b, near_b);
  world->Tick();

  EXPECT_GE(sync->per_viewer.EnterStepCount(a->GetId(), b->GetId()), 1u);
  EXPECT_GE(sync->per_viewer.EnterStepCount(b->GetId(), a->GetId()), 1u);
}
