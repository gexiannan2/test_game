// 账号/角色全栈路径（无网络）：OnUserLogin 到选角、删角、再选角。
#include "test_harness.h"

#include "ecs/components/account_component.h"
#include "ecs/components/connection_component.h"
#include "ecs/components/role_component.h"
#include "ecs/entity/player_entity.h"
#include "session/system.h"

namespace {

EntityPtr MakeEntity(uint64_t id) {
  return std::make_shared<PlayerEntity>(id);
}

}  // namespace

GAME_TEST_SUITE(ProductionFullStackTest);

GAME_TEST(ProductionFullStackTest, FullAccountRoleFlow) {
  auto conn_entity = MakeEntity(300);
  uint64_t session = SessionService::OnUserLogin(conn_entity, "fullstack_uid", "tok", 1, 70001);

  EXPECT_EQ(session, 70001u);
  EXPECT_EQ(static_cast<int>(conn_entity->GetState()),
            static_cast<int>(Entity::State::kLoggedIn));
  EXPECT_EQ(PlayerEntitySystem::Instance().FindByUid("fullstack_uid"), conn_entity);
  EXPECT_EQ(PlayerEntitySystem::Instance().FindBySessionId(70001), conn_entity);

  SessionService::OnRoleListReq(conn_entity);
  SessionService::OnRoleCreate(conn_entity, 60001, "FullHero", 1, 1);
  EXPECT_TRUE(SessionService::OnRoleLogin(conn_entity, 60001));
  EXPECT_EQ(static_cast<int>(conn_entity->GetState()),
            static_cast<int>(Entity::State::kRoleSelected));

  SessionService::OnRoleCreate(conn_entity, 60002, "AltHero", 2, 2);
  SessionService::OnRoleDelete(conn_entity, 60001);

  EXPECT_EQ(PlayerEntitySystem::Instance().FindByUid("fullstack_uid"), conn_entity);
  EXPECT_EQ(PlayerEntitySystem::Instance().FindByRoleId(60001), nullptr);
  EXPECT_TRUE(SessionService::OnRoleLogin(conn_entity, 60002));
  EXPECT_EQ(conn_entity->GetComponent<RoleComponent>()->role_id_, 60002u);
}

// 顶号后新连接临时 entity Cleanup 不影响缓存实体
GAME_TEST(ProductionFullStackTest, ReLoginSimulationNewConnectionEntity) {
  auto old_conn = MakeEntity(301);
  SessionService::OnUserLogin(old_conn, "relogin_uid", "t1", 1, 70002);
  SessionService::OnRoleCreate(old_conn, 60003, "Relog", 1, 1);
  EXPECT_TRUE(SessionService::OnRoleLogin(old_conn, 60003));

  // 模拟新 TCP 连接创建的临时 entity（仅有 ConnectionComponent，与 OnConnection 一致）
  auto new_conn = MakeEntity(302);
  new_conn->AddComponent<ConnectionComponent>();

  EXPECT_EQ(PlayerEntitySystem::Instance().FindByUid("relogin_uid"), old_conn);
  PlayerEntitySystem::Instance().CleanupEntity(new_conn);

  EXPECT_EQ(PlayerEntitySystem::Instance().FindByUid("relogin_uid"), old_conn);
  EXPECT_EQ(PlayerEntitySystem::Instance().FindByRoleId(60003), old_conn);
  EXPECT_TRUE(SessionService::OnRoleLogin(old_conn, 60003));
}
