// Handler 业务逻辑单元测试（无网络、无 GameServer）。
#include "test_harness.h"

#include "ecs/components/account_component.h"
#include "ecs/components/map_component.h"
#include "ecs/components/role_component.h"
#include "ecs/components/transform_component.h"
#include "ecs/entity/player_entity.h"
#include "session/system.h"

namespace {

EntityPtr MakeEntity(uint64_t id) {
  return std::make_shared<PlayerEntity>(id);
}

void SetupLoggedInAccount(const EntityPtr& entity, const std::string& uid,
                          uint64_t session_id) {
  entity->AddComponent<AccountComponent>();
  auto* acc = entity->GetComponent<AccountComponent>();
  acc->uid_ = uid;
  acc->session_id_ = session_id;
  acc->token_ = "token";
  acc->channel_id_ = 1;
  entity->SetState(Entity::State::kLoggedIn);
  PlayerEntitySystem::Instance().RegisterByUid(uid, entity);
  PlayerEntitySystem::Instance().RegisterBySessionId(session_id, entity);
}

}  // namespace

GAME_TEST_SUITE(HandlerLogicTest);

GAME_TEST(HandlerLogicTest, UnregisterKeepsUidAfterRoleDelete) {
  auto entity = MakeEntity(1);
  SetupLoggedInAccount(entity, "uid_multi_role", 10001);

  System::OnRoleCreate(entity, 20001, "HeroA", 1, 1);
  System::OnRoleCreate(entity, 20002, "HeroB", 2, 2);

  EXPECT_EQ(PlayerEntitySystem::Instance().FindByUid("uid_multi_role"), entity);
  EXPECT_NE(PlayerEntitySystem::Instance().FindByRoleId(20001), nullptr);
  EXPECT_NE(PlayerEntitySystem::Instance().FindByRoleId(20002), nullptr);

  System::OnRoleDelete(entity, 20001);

  EXPECT_EQ(PlayerEntitySystem::Instance().FindByUid("uid_multi_role"), entity);
  EXPECT_EQ(PlayerEntitySystem::Instance().FindBySessionId(10001), entity);
  EXPECT_EQ(PlayerEntitySystem::Instance().FindByRoleId(20001), nullptr);
  EXPECT_EQ(PlayerEntitySystem::Instance().FindByRoleId(20002), entity);

  auto* role = entity->GetComponent<RoleComponent>();
  EXPECT_EQ(role->all_roles_.size(), 1u);
  EXPECT_EQ(role->all_roles_.front().role_id, 20002u);
}

// 模拟 GameHandler 首次进图：零位 Transform + 无 MapComponent → 写出生点
GAME_TEST(HandlerLogicTest, FirstEnterUsesBornPosDespiteDefaultTransform) {
  auto entity = MakeEntity(60);
  // 模拟 OnConnection 创建的 PlayerEntity：已有零位 Transform
  entity->AddComponent<TransformComponent>();
  EXPECT_EQ(entity->GetPosition().GetX(), 0.0f);

  const bool first_enter = !entity->HasComponent<MapComponent>();
  EXPECT_TRUE(first_enter);
  if (first_enter) {
    entity->GetComponent<TransformComponent>()->pos_ =
        JPH::Vec3(333.0f, 18.0f, 415.45f);
    entity->AddComponent<MapComponent>();
  }
  EXPECT_NEAR(entity->GetPosition().GetX(), 333.0f, 0.01f);
}

GAME_TEST(HandlerLogicTest, OnRoleLoginRejectsInvalidRoleId) {
  auto entity = MakeEntity(2);
  SetupLoggedInAccount(entity, "uid_invalid_login", 10002);
  System::OnRoleCreate(entity, 20003, "Valid", 1, 1);

  bool ok = System::OnRoleLogin(entity, 99999);
  EXPECT_FALSE(ok);
  EXPECT_EQ(static_cast<int>(entity->GetState()),
            static_cast<int>(Entity::State::kLoggedIn));
  EXPECT_NE(entity->GetComponent<RoleComponent>()->role_id_, 99999u);
}

GAME_TEST(HandlerLogicTest, OnRoleLoginAcceptsValidRoleId) {
  auto entity = MakeEntity(3);
  SetupLoggedInAccount(entity, "uid_valid_login", 10003);
  System::OnRoleCreate(entity, 20004, "ValidHero", 1, 1);

  bool ok = System::OnRoleLogin(entity, 20004);
  EXPECT_TRUE(ok);
  EXPECT_EQ(static_cast<int>(entity->GetState()),
            static_cast<int>(Entity::State::kRoleSelected));
  EXPECT_EQ(entity->GetComponent<RoleComponent>()->role_id_, 20004u);
  EXPECT_EQ(PlayerEntitySystem::Instance().FindByRoleId(20004), entity);
}

GAME_TEST(HandlerLogicTest, DeleteAllRolesClearsRoleIndexButKeepsUid) {
  auto entity = MakeEntity(4);
  SetupLoggedInAccount(entity, "uid_delete_all", 10004);
  System::OnRoleCreate(entity, 20005, "Only", 1, 1);
  System::OnRoleDelete(entity, 20005);

  EXPECT_EQ(PlayerEntitySystem::Instance().FindByUid("uid_delete_all"), entity);
  EXPECT_EQ(PlayerEntitySystem::Instance().FindByRoleId(20005), nullptr);
  EXPECT_EQ(entity->GetComponent<RoleComponent>()->role_id_, 0u);
}

GAME_TEST(HandlerLogicTest, RandomNameReturnsNonEmptyForValidSex) {
  std::string name = System::OnRandomNameReq(1);
  EXPECT_TRUE(!name.empty());
  std::string name2 = System::OnRandomNameReq(2);
  EXPECT_TRUE(!name2.empty());
}

GAME_TEST(HandlerLogicTest, RoleMarkLastConsistency) {
  auto entity = MakeEntity(5);
  SetupLoggedInAccount(entity, "uid_mark_last", 10005);
  System::OnRoleCreate(entity, 20006, "A", 1, 1);
  System::OnRoleCreate(entity, 20007, "B", 2, 2);

  auto* role = entity->GetComponent<RoleComponent>();
  int last_count = 0;
  for (const auto& r : role->all_roles_) {
    if (r.is_last) ++last_count;
  }
  EXPECT_EQ(last_count, 1);

  bool ok = System::OnRoleLogin(entity, 20006);
  EXPECT_TRUE(ok);
  last_count = 0;
  for (const auto& r : role->all_roles_) {
    if (r.is_last) ++last_count;
    if (r.role_id == 20006) EXPECT_TRUE(r.is_last);
    if (r.role_id == 20007) EXPECT_FALSE(r.is_last);
  }
  EXPECT_EQ(last_count, 1);
}

GAME_TEST(HandlerLogicTest, UnregisterStressManyRoles) {
  auto entity = MakeEntity(6);
  SetupLoggedInAccount(entity, "uid_stress", 10006);

  constexpr int kRoles = 64;
  for (int i = 0; i < kRoles; ++i) {
    System::OnRoleCreate(entity, 30000u + static_cast<uint64_t>(i),
                         "R" + std::to_string(i), 1, 1);
  }
  EXPECT_EQ(PlayerEntitySystem::Instance().FindByUid("uid_stress"), entity);

  for (int i = 0; i < kRoles; ++i) {
    System::OnRoleDelete(entity, 30000u + static_cast<uint64_t>(i));
    EXPECT_EQ(PlayerEntitySystem::Instance().FindByUid("uid_stress"), entity);
    EXPECT_EQ(PlayerEntitySystem::Instance().FindByRoleId(30000u + static_cast<uint64_t>(i)),
              nullptr);
  }
}
