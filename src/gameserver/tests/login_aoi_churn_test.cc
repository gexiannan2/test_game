// 登陆→选角→进图→AOI→离图→重进、以及高频上下线/索引 churn 高强度单元测试。
#include "test_harness.h"
#include "test_map_invariants.h"
#include "test_world_invariants.h"

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/aoi_def.h"
#include "ecs/components/account_component.h"
#include "ecs/components/map_component.h"
#include "ecs/components/player_data_component.h"
#include "ecs/components/role_component.h"
#include "ecs/components/transform_component.h"
#include "ecs/entity/player_entity.h"
#include "ecs/systems/player_entity_system.h"
#include "ecs/systems/world_system.h"
#include "session/system.h"

namespace {

using test::AssertWorldConsistent;
using test::GridCenter;
using test::MakeWorld;

EntityPtr MakePlayerShell(uint64_t id) {
  return std::make_shared<PlayerEntity>(id);
}

void BindAccount(const EntityPtr& e, const std::string& uid, uint64_t sid) {
  if (!e->HasComponent<AccountComponent>()) {
    e->AddComponent<AccountComponent>();
  }
  auto* acc = e->GetComponent<AccountComponent>();
  acc->uid_ = uid;
  acc->session_id_ = sid;
  acc->token_ = "tok";
  acc->channel_id_ = 1;
  e->SetState(Entity::State::kLoggedIn);
  PlayerEntitySystem::Instance().RegisterByUid(uid, e);
  PlayerEntitySystem::Instance().RegisterBySessionId(sid, e);
}

// 模拟 login → role_create → role_login → enter_game（无网络）
EntityPtr LoginSelectAndEnter(WorldSystem& world, uint64_t entity_id,
                              const std::string& uid, uint64_t session_id,
                              uint64_t role_id, const Vector3D& born) {
  auto e = MakePlayerShell(entity_id);
  BindAccount(e, uid, session_id);
  SessionService::OnRoleCreate(e, role_id, "Hero" + std::to_string(role_id), 1, 1);
  EXPECT_TRUE(SessionService::OnRoleLogin(e, role_id));
  EXPECT_EQ(static_cast<int>(e->GetState()),
            static_cast<int>(Entity::State::kRoleSelected));

  e->AddComponent<TransformComponent>();
  e->GetComponent<TransformComponent>()->pos_ = born;
  e->AddComponent<MapComponent>();
  e->GetComponent<MapComponent>()->map_cfg_id_ = 1;
  e->SetState(Entity::State::kInGame);
  world.EnterMap(e);
  EXPECT_TRUE(e->IsInMap());
  EXPECT_TRUE(world.IsWatcher(e->GetId()));
  return e;
}

// 模拟断线：离图 + 清连接态，保留 uid/role 索引（与 game_server disconnect 一致）
void SimulateDisconnect(WorldSystem& world, const EntityPtr& e) {
  if (e->IsInMap()) {
    world.LeaveMap(e);
  }
  e->SetState(Entity::State::kDisconnected);
  EXPECT_FALSE(e->IsInMap());
  EXPECT_FALSE(world.IsWatcher(e->GetId()));
}

// 模拟重连后再次选角进图
void SimulateReenter(WorldSystem& world, const EntityPtr& e, const Vector3D& pos) {
  e->SetState(Entity::State::kRoleSelected);
  if (!e->HasComponent<TransformComponent>()) {
    e->AddComponent<TransformComponent>();
  }
  e->GetComponent<TransformComponent>()->pos_ = pos;
  if (!e->HasComponent<MapComponent>()) {
    e->AddComponent<MapComponent>();
  }
  if (e->IsInMap()) {
    world.LeaveMap(e);
  }
  e->SetState(Entity::State::kInGame);
  world.EnterMap(e);
  EXPECT_TRUE(e->IsInMap());
  EXPECT_TRUE(world.IsWatcher(e->GetId()));
}

}  // namespace

GAME_TEST_SUITE(LoginAoiChurnTest);

// 单玩家：登陆选角进图 → 移动跨 AOI → 断线 → 再进图，足迹/视野干净
GAME_TEST(LoginAoiChurnTest, SinglePlayerLoginEnterLeaveReenter) {
  auto world = MakeWorld();
  const Vector3D born = GridCenter(20, 20);
  auto p = LoginSelectAndEnter(*world, 1, "churn_uid_1", 70001, 80001, born);

  int appear = 0;
  int disappear = 0;
  world->Aoi().SetEntityEnterCallback(
      [&](uint64_t, const std::vector<uint64_t>& ids) {
        appear += static_cast<int>(ids.size());
      });
  world->Aoi().SetEntityLeaveCallback(
      [&](uint64_t, const std::vector<uint64_t>& ids) {
        disappear += static_cast<int>(ids.size());
      });

  world->MoveEntity(p, GridCenter(40, 40), EntityPropertyType::kMove);
  p->SetMoveState(JPH::Quat::sIdentity(), JPH::Quat::sIdentity(),
                  JPH::Vec3(1, 0, 0), EntityPropertyType::kMove);

  SimulateDisconnect(*world, p);
  EXPECT_EQ(PlayerEntitySystem::Instance().FindByUid("churn_uid_1"), p);
  EXPECT_EQ(PlayerEntitySystem::Instance().FindByRoleId(80001), p);

  appear = 0;
  SimulateReenter(*world, p, born);
  EXPECT_EQ(world->Map().AllocatedGridCount(), 1u);

  world->LeaveMap(p);
  EXPECT_EQ(world->Map().AllocatedGridCount(), 0u);
  AssertWorldConsistent(*world, {p});
}

// 双玩家互见：一人离图后对方 disappear；再进图后互见恢复
GAME_TEST(LoginAoiChurnTest, TwoPlayersAoiAppearDisappearOnLeaveReenter) {
  auto world = MakeWorld();
  const Vector3D born = GridCenter(10, 10);
  auto a = LoginSelectAndEnter(*world, 1, "aoi_a", 71001, 81001, born);
  auto b = LoginSelectAndEnter(*world, 2, "aoi_b", 71002, 81002, born);

  auto vis_a = world->GetVisibleEntities(a->GetId());
  auto vis_b = world->GetVisibleEntities(b->GetId());
  EXPECT_TRUE(std::find(vis_a.begin(), vis_a.end(), b->GetId()) != vis_a.end());
  EXPECT_TRUE(std::find(vis_b.begin(), vis_b.end(), a->GetId()) != vis_b.end());

  int disappear_to_a = 0;
  world->Aoi().SetEntityLeaveCallback(
      [&](uint64_t viewer, const std::vector<uint64_t>& ids) {
        if (viewer == a->GetId()) {
          disappear_to_a += static_cast<int>(ids.size());
        }
      });

  SimulateDisconnect(*world, b);
  EXPECT_GE(disappear_to_a, 1);

  vis_a = world->GetVisibleEntities(a->GetId());
  EXPECT_TRUE(std::find(vis_a.begin(), vis_a.end(), b->GetId()) == vis_a.end());

  int appear_to_a = 0;
  world->Aoi().SetEntityEnterCallback(
      [&](uint64_t viewer, const std::vector<uint64_t>& ids) {
        if (viewer == a->GetId()) {
          appear_to_a += static_cast<int>(ids.size());
        }
      });
  SimulateReenter(*world, b, born);
  EXPECT_GE(appear_to_a, 1);

  vis_a = world->GetVisibleEntities(a->GetId());
  EXPECT_TRUE(std::find(vis_a.begin(), vis_a.end(), b->GetId()) != vis_a.end());

  world->LeaveMap(a);
  world->LeaveMap(b);
  EXPECT_EQ(world->Map().AllocatedGridCount(), 0u);
}

// 高频：N 人反复进离图 + 随机移动，足迹与 registry 不变量
GAME_TEST(LoginAoiChurnTest, MassEnterLeaveMoveChurnInvariants) {
  auto world = MakeWorld();
  constexpr int kPlayers = 64;
  constexpr int kRounds = 40;
  const Vector3D born = GridCenter(25, 25);

  std::vector<EntityPtr> players;
  players.reserve(kPlayers);
  for (int i = 0; i < kPlayers; ++i) {
    players.push_back(LoginSelectAndEnter(
        *world, static_cast<uint64_t>(1000 + i),
        "mass_uid_" + std::to_string(i), 72000u + static_cast<uint64_t>(i),
        82000u + static_cast<uint64_t>(i), born));
  }
  AssertWorldConsistent(*world, players);

  std::mt19937 rng(20260805);
  std::uniform_int_distribution<int> pick(0, kPlayers - 1);
  std::uniform_int_distribution<int> cell(5, 45);
  std::uniform_int_distribution<int> action(0, 2);  // 0 leave, 1 enter, 2 move

  for (int r = 0; r < kRounds; ++r) {
    for (int n = 0; n < kPlayers / 2; ++n) {
      auto& e = players[pick(rng)];
      switch (action(rng)) {
        case 0:
          if (e->IsInMap()) {
            world->LeaveMap(e);
            e->SetState(Entity::State::kDisconnected);
          }
          break;
        case 1:
          if (!e->IsInMap()) {
            SimulateReenter(*world, e, GridCenter(cell(rng), cell(rng)));
          }
          break;
        default:
          if (e->IsInMap()) {
            world->MoveEntity(e, GridCenter(cell(rng), cell(rng)), EntityPropertyType::kMove);
          }
          break;
      }
    }
    AssertWorldConsistent(*world, players);
  }

  for (auto& e : players) {
    if (e->IsInMap()) {
      world->LeaveMap(e);
    }
  }
  EXPECT_EQ(world->Map().AllocatedGridCount(), 0u);
}

// 临时连接实体 CleanupEntity 不得误删缓存账号索引（顶号/重连场景）
GAME_TEST(LoginAoiChurnTest, RapidOnlineOfflineTempEntityCleanup) {
  constexpr int kCycles = 200;
  auto cached = MakePlayerShell(9000);
  BindAccount(cached, "stable_uid", 73001);
  SessionService::OnRoleCreate(cached, 83001, "Stable", 1, 1);
  EXPECT_TRUE(SessionService::OnRoleLogin(cached, 83001));

  for (int i = 0; i < kCycles; ++i) {
    auto temp = MakePlayerShell(static_cast<uint64_t>(9100 + i));
    temp->AddComponent<AccountComponent>();
    temp->GetComponent<AccountComponent>()->uid_ = "temp_" + std::to_string(i);
    PlayerEntitySystem::Instance().CleanupEntity(temp);

    EXPECT_EQ(PlayerEntitySystem::Instance().FindByUid("stable_uid"), cached);
    EXPECT_EQ(PlayerEntitySystem::Instance().FindBySessionId(73001), cached);
    EXPECT_EQ(PlayerEntitySystem::Instance().FindByRoleId(83001), cached);
  }
}

// 同账号顶角色：玩法组件须迁移，新实体不得丢失位置/Map 标记
GAME_TEST(LoginAoiChurnTest, RoleReplaceTransfersGameplayComponents) {
  auto old_ent = MakePlayerShell(1);
  BindAccount(old_ent, "replace_uid", 74001);
  SessionService::OnRoleCreate(old_ent, 84001, "Old", 1, 1);
  EXPECT_TRUE(SessionService::OnRoleLogin(old_ent, 84001));

  old_ent->AddComponent<TransformComponent>();
  old_ent->GetComponent<TransformComponent>()->pos_ =
      JPH::Vec3(111.f, 22.f, 333.f);
  old_ent->AddComponent<MapComponent>();
  old_ent->GetComponent<MapComponent>()->map_cfg_id_ = 7;
  old_ent->AddComponent<PlayerDataComponent>();
  old_ent->GetComponent<PlayerDataComponent>()->SetDirty();

  auto new_ent = MakePlayerShell(2);
  BindAccount(new_ent, "replace_uid", 74002);
  // 模拟 handler：先校验再迁移（OnRoleLogin 内部 Transfer + Cleanup）
  EXPECT_TRUE(SessionService::OnRoleLogin(new_ent, 84001));

  EXPECT_EQ(PlayerEntitySystem::Instance().FindByRoleId(84001), new_ent);
  EXPECT_EQ(PlayerEntitySystem::Instance().FindByUid("replace_uid"), new_ent);
  EXPECT_TRUE(new_ent->HasComponent<TransformComponent>());
  EXPECT_NEAR(new_ent->GetPosition().GetX(), 111.f, 0.01f);
  EXPECT_TRUE(new_ent->HasComponent<MapComponent>());
  EXPECT_EQ(new_ent->GetComponent<MapComponent>()->map_cfg_id_, 7u);
  EXPECT_TRUE(new_ent->HasComponent<PlayerDataComponent>());
  EXPECT_TRUE(new_ent->GetComponent<PlayerDataComponent>()->IsDirty());
}

// 模拟 LeaveMap 前再 EnterMap（reconnect / enter_game 守卫）幂等
GAME_TEST(LoginAoiChurnTest, LeaveBeforeEnterMapGuardIdempotent) {
  auto world = MakeWorld();
  const Vector3D born = GridCenter(12, 12);
  auto p = LoginSelectAndEnter(*world, 1, "guard_uid", 75001, 85001, born);
  EXPECT_TRUE(p->IsInMap());

  // 残留 IsInMap 时直接 EnterMap 会短路；须先 Leave
  world->LeaveMap(p);
  world->EnterMap(p);
  EXPECT_TRUE(p->IsInMap());
  EXPECT_TRUE(world->IsWatcher(p->GetId()));

  // 二次 Leave/Enter
  if (p->IsInMap()) {
    world->LeaveMap(p);
  }
  world->EnterMap(p);
  EXPECT_EQ(world->Map().AllocatedGridCount(), 1u);

  world->LeaveMap(p);
  EXPECT_EQ(world->Map().AllocatedGridCount(), 0u);
}

// 多账号并行生命周期：索引隔离 + 进离图交叉
GAME_TEST(LoginAoiChurnTest, MultiAccountParallelLifecycleIsolation) {
  auto world = MakeWorld();
  constexpr int kAccounts = 32;
  std::vector<EntityPtr> ents;
  ents.reserve(kAccounts);
  const Vector3D born = GridCenter(30, 30);

  for (int i = 0; i < kAccounts; ++i) {
    ents.push_back(LoginSelectAndEnter(
        *world, static_cast<uint64_t>(2000 + i),
        "iso_uid_" + std::to_string(i), 76000u + static_cast<uint64_t>(i),
        86000u + static_cast<uint64_t>(i), born));
  }

  for (int i = 0; i < kAccounts; ++i) {
    EXPECT_EQ(PlayerEntitySystem::Instance().FindByUid("iso_uid_" + std::to_string(i)),
              ents[i]);
    EXPECT_EQ(PlayerEntitySystem::Instance().FindByRoleId(86000u + static_cast<uint64_t>(i)),
              ents[i]);
  }

  // 偶数离线，奇数留下；再让偶数重进
  for (int i = 0; i < kAccounts; i += 2) {
    SimulateDisconnect(*world, ents[i]);
  }
  for (int i = 1; i < kAccounts; i += 2) {
    EXPECT_TRUE(ents[i]->IsInMap());
  }
  for (int i = 0; i < kAccounts; i += 2) {
    SimulateReenter(*world, ents[i], born);
  }

  AssertWorldConsistent(*world, ents);
  for (auto& e : ents) {
    world->LeaveMap(e);
  }
  EXPECT_EQ(world->Map().AllocatedGridCount(), 0u);
}
