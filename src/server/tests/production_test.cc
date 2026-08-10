// 生产级索引一致性与 AOI 回归（PlayerEntitySystem CRUD、出生点移回）。
#include "test_harness.h"

#include "ecs/components/account_component.h"
#include "ecs/components/map_component.h"
#include "ecs/components/transform_component.h"
#include "ecs/entity/player_entity.h"
#include "common/aoi_def.h"
#include "session/system.h"
#include "ecs/systems/world_system.h"

namespace {

EntityPtr MakeEntity(uint64_t id) {
  return std::make_shared<PlayerEntity>(id);
}

void RegisterAccount(const EntityPtr& e, const std::string& uid, uint64_t sid) {
  e->AddComponent<AccountComponent>();
  auto* acc = e->GetComponent<AccountComponent>();
  acc->uid_ = uid;
  acc->session_id_ = sid;
  e->SetState(Entity::State::kLoggedIn);
  PlayerEntitySystem::Instance().RegisterByUid(uid, e);
  PlayerEntitySystem::Instance().RegisterBySessionId(sid, e);
}

}  // namespace

GAME_TEST_SUITE(ProductionTest);

GAME_TEST(ProductionTest, ConcurrentRoleCreateDeleteIndexIntegrity) {
  auto entity = MakeEntity(100);
  RegisterAccount(entity, "prod_uid_1", 50001);

  constexpr int kN = 128;
  for (int i = 0; i < kN; ++i) {
    uint64_t rid = 40000u + static_cast<uint64_t>(i);
    System::OnRoleCreate(entity, rid, "P" + std::to_string(i), 1, 1);
    EXPECT_EQ(PlayerEntitySystem::Instance().FindByRoleId(rid), entity);
    EXPECT_EQ(PlayerEntitySystem::Instance().FindByUid("prod_uid_1"), entity);
  }

  for (int i = 0; i < kN; ++i) {
    uint64_t rid = 40000u + static_cast<uint64_t>(i);
    System::OnRoleDelete(entity, rid);
    EXPECT_EQ(PlayerEntitySystem::Instance().FindByRoleId(rid), nullptr);
    EXPECT_EQ(PlayerEntitySystem::Instance().FindByUid("prod_uid_1"), entity);
  }
}

GAME_TEST(ProductionTest, MultiAccountUidIsolation) {
  auto e1 = MakeEntity(101);
  auto e2 = MakeEntity(102);
  RegisterAccount(e1, "prod_uid_a", 50002);
  RegisterAccount(e2, "prod_uid_b", 50003);

  System::OnRoleCreate(e1, 40001, "A1", 1, 1);
  System::OnRoleCreate(e2, 40002, "B1", 2, 2);

  EXPECT_EQ(PlayerEntitySystem::Instance().FindByUid("prod_uid_a"), e1);
  EXPECT_EQ(PlayerEntitySystem::Instance().FindByUid("prod_uid_b"), e2);
  EXPECT_NE(PlayerEntitySystem::Instance().FindByRoleId(40001), e2);
  EXPECT_NE(PlayerEntitySystem::Instance().FindByRoleId(40002), e1);

  bool deny = System::OnRoleLogin(e2, 40001);
  EXPECT_FALSE(deny);
  // 拒绝后受害者索引与状态不得被破坏（handler 踢人前须同样先做 uid 校验）
  EXPECT_EQ(PlayerEntitySystem::Instance().FindByRoleId(40001), e1);
  EXPECT_EQ(PlayerEntitySystem::Instance().FindByUid("prod_uid_a"), e1);
  EXPECT_NE(static_cast<int>(e1->GetState()),
            static_cast<int>(Entity::State::kDisconnected));
}

GAME_TEST(ProductionTest, RoleLoginRoundTripAfterPartialDelete) {
  auto entity = MakeEntity(103);
  RegisterAccount(entity, "prod_uid_round", 50004);
  System::OnRoleCreate(entity, 40010, "Keep", 1, 1);
  System::OnRoleCreate(entity, 40011, "Drop", 2, 2);
  System::OnRoleDelete(entity, 40011);

  EXPECT_TRUE(System::OnRoleLogin(entity, 40010));
  EXPECT_EQ(entity->GetComponent<RoleComponent>()->name_, "Keep");
  EXPECT_EQ(PlayerEntitySystem::Instance().FindByRoleId(40010), entity);
}

// 与 game_handler 出生坐标一致：同点移远再移回
GAME_TEST(ProductionTest, BornPosMoveBackLikeGameServer) {
  constexpr float kBornX = 333.0f;
  constexpr float kBornY = 18.0f;
  constexpr float kBornZ = 415.45f;
  auto world = WorldSystem::Create(SceneRegionType::kMap);
  world->Init();

  int appear_to_a = 0;
  int disappear_to_a = 0;
  world->Aoi().SetEntityEnterCallback(
      [&](uint64_t viewer, const std::vector<uint64_t>& ids) {
        if (viewer == 1) appear_to_a += static_cast<int>(ids.size());
      });
  world->Aoi().SetEntityLeaveCallback(
      [&](uint64_t viewer, const std::vector<uint64_t>& ids) {
        if (viewer == 1) disappear_to_a += static_cast<int>(ids.size());
      });

  auto spawn_at_born = [&](uint64_t id) {
    auto e = std::make_shared<PlayerEntity>(id);
    e->AddComponent<RoleComponent>();
    e->GetComponent<RoleComponent>()->role_id_ = id * 100;
    e->AddComponent<MapComponent>();
    e->GetComponent<MapComponent>()->map_cfg_id_ = 1;
    e->AddComponent<TransformComponent>();
    auto* tfm = e->GetComponent<TransformComponent>();
    tfm->pos_ = JPH::Vec3(kBornX, kBornY, kBornZ);
    world->EnterMap(e);
    return e;
  };

  auto a = spawn_at_born(1);
  auto b = spawn_at_born(2);
  EXPECT_TRUE(a && b);

  disappear_to_a = 0;
  world->MoveEntity(b, JPH::Vec3(1000.0f, 20.0f, 1000.0f));
  b->SetMoveState(JPH::Quat::sIdentity(), JPH::Quat::sIdentity(),
                  JPH::Vec3(1, 0, 0));
  EXPECT_TRUE(disappear_to_a >= 1);

  appear_to_a = 0;
  world->MoveEntity(b, JPH::Vec3(kBornX, kBornY, kBornZ));
  b->SetMoveState(JPH::Quat::sIdentity(), JPH::Quat::sIdentity(),
                  JPH::Vec3(1, 0, 0));
  EXPECT_TRUE(appear_to_a >= 1);
}
