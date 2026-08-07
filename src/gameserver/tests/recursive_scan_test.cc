// 角色/账号索引递归扫描不变量（is_last 唯一、CleanupEntity 不误删缓存）。
#include "test_harness.h"

#include "ecs/components/account_component.h"
#include "ecs/components/role_component.h"
#include "ecs/entity/player_entity.h"
#include "session/system.h"

namespace {

EntityPtr MakeEntity(uint64_t id) {
  return std::make_shared<PlayerEntity>(id);
}

// 扫描 role 列表：role_id>0、is_last 至多一个、当前选中角色必须在 all_roles_ 中
void ScanRoleListInvariants(const RoleComponent* role) {
  EXPECT_TRUE(role != nullptr);
  int last_count = 0;
  for (const auto& info : role->all_roles_) {
    EXPECT_GT(info.role_id, 0u);
    if (info.is_last) ++last_count;
  }
  EXPECT_LE(last_count, 1);
  if (!role->all_roles_.empty() && role->role_id_ != 0) {
    bool current_in_list = false;
    for (const auto& info : role->all_roles_) {
      if (info.role_id == role->role_id_) current_in_list = true;
    }
    EXPECT_TRUE(current_in_list);
  }
}

}  // namespace

GAME_TEST_SUITE(RecursiveScanTest);

GAME_TEST(RecursiveScanTest, ScanRoleLifecycleInvariants) {
  auto entity = MakeEntity(200);
  entity->AddComponent<AccountComponent>();
  auto* acc = entity->GetComponent<AccountComponent>();
  acc->uid_ = "scan_uid";
  acc->session_id_ = 60001;
  entity->SetState(Entity::State::kLoggedIn);
  PlayerEntitySystem::Instance().RegisterByUid(acc->uid_, entity);
  PlayerEntitySystem::Instance().RegisterBySessionId(acc->session_id_, entity);

  for (int step = 0; step < 32; ++step) {
    uint64_t rid = 50000u + static_cast<uint64_t>(step);
    SessionService::OnRoleCreate(entity, rid, "S" + std::to_string(step), 1, 1);
    ScanRoleListInvariants(entity->GetComponent<RoleComponent>());
    EXPECT_EQ(PlayerEntitySystem::Instance().FindByUid("scan_uid"), entity);

    EXPECT_TRUE(SessionService::OnRoleLogin(entity, rid));
    ScanRoleListInvariants(entity->GetComponent<RoleComponent>());
    EXPECT_EQ(entity->GetComponent<RoleComponent>()->role_id_, rid);

    if (step % 3 == 0) {
      SessionService::OnRoleDelete(entity, rid);
      ScanRoleListInvariants(entity->GetComponent<RoleComponent>());
      EXPECT_EQ(PlayerEntitySystem::Instance().FindByRoleId(rid), nullptr);
      EXPECT_EQ(PlayerEntitySystem::Instance().FindByUid("scan_uid"), entity);
    }
  }
}

GAME_TEST(RecursiveScanTest, ScanIndexMapsAfterCleanupEntity) {
  auto temp = MakeEntity(201);
  auto cached = MakeEntity(202);
  cached->AddComponent<AccountComponent>();
  auto* acc = cached->GetComponent<AccountComponent>();
  acc->uid_ = "scan_cleanup";
  acc->session_id_ = 60002;
  cached->SetState(Entity::State::kLoggedIn);
  PlayerEntitySystem::Instance().RegisterByUid(acc->uid_, cached);
  PlayerEntitySystem::Instance().RegisterBySessionId(acc->session_id_, cached);
  SessionService::OnRoleCreate(cached, 50099, "Cached", 1, 1);

  temp->AddComponent<AccountComponent>();
  temp->GetComponent<AccountComponent>()->uid_ = "temp";
  PlayerEntitySystem::Instance().CleanupEntity(temp);

  EXPECT_EQ(PlayerEntitySystem::Instance().FindByUid("scan_cleanup"), cached);
  EXPECT_EQ(PlayerEntitySystem::Instance().FindByRoleId(50099), cached);
}
