// 角色列表、创建、删除、选角、随机名。

#include "session/system.h"

#include <ctime>
#include <string>

#include "ecs/component_base/component_base.h"
#include "ecs/components/role_component.h"
#include "ecs/components/account_component.h"
#include "ecs/components/transform_component.h"
#include "ecs/entity/entity.h"
#include "session/random_name_generator.h"
#include "zrpc/base/logger.h"

void SessionService::OnRoleListReq(const EntityPtr& entity) {
  auto* acc = entity->GetComponent<AccountComponent>();
  if (acc) {
    auto existing = PlayerEntitySystem::Instance().FindByUid(acc->uid_);
    if (existing && existing != entity) {
      auto* old_role = existing->GetComponent<RoleComponent>();
      if (old_role && old_role->role_id_ != 0) {
        if (!entity->HasComponent<RoleComponent>()) {
          entity->AddComponent<RoleComponent>();
        }

        *entity->GetComponent<RoleComponent>() = *old_role;
        LOG_WARN << "role list req: uid entity mismatch existing="
                 << existing->GetId() << " current=" << entity->GetId()
                 << " copied roles without rebind role_id="
                 << old_role->role_id_
                 << " role_count=" << old_role->all_roles_.size();
        return;
      }
    }
  }

  if (!entity->HasComponent<RoleComponent>()) {
    entity->AddComponent<RoleComponent>();
  }
  auto* role = entity->GetComponent<RoleComponent>();
  LOG_INFO << "role list req: current role_id=" << role->role_id_
           << " role_count=" << role->all_roles_.size();
}

void SessionService::OnRoleCreate(const EntityPtr& entity, uint64_t role_id,
                               const std::string& name, uint32_t sex,
                               uint32_t job) {
  if (!entity->HasComponent<RoleComponent>()) {
    entity->AddComponent<RoleComponent>();
  }
  auto* role = entity->GetComponent<RoleComponent>();

  RoleInfo info;
  info.role_id = role_id;
  info.name = name;
  info.sex = sex;
  info.job = job;
  info.create_time = static_cast<uint64_t>(std::time(nullptr));
  info.is_last = true;
  role->MarkLast(role_id);
  role->all_roles_.push_back(info);
  role->SetCurrent(info);

  PlayerEntitySystem::Instance().RegisterByRoleId(role_id, entity);

  LOG_INFO << "create role: name=" << name
           << " role_id=" << role_id
           << " total_roles=" << role->all_roles_.size();
}

void SessionService::OnRoleDelete(const EntityPtr& entity, uint64_t role_id) {
  auto* role = entity->GetComponent<RoleComponent>();
  if (role) {
    for (auto it = role->all_roles_.begin(); it != role->all_roles_.end(); ++it) {
      if (it->role_id == role_id) {
        role->all_roles_.erase(it);
        break;
      }
    }
    PlayerEntitySystem::Instance().Unregister(role_id);
    if (role->role_id_ == role_id) {
      if (!role->all_roles_.empty()) {
        // 回落到列表末尾（较新创建），并 MarkLast
        const RoleInfo& fallback = role->all_roles_.back();
        role->MarkLast(fallback.role_id);
        role->SetCurrent(fallback);
      } else {
        role->role_id_ = 0;
        role->name_.clear();
        role->sex_  = 0;
        role->job_  = 0;
        role->level_ = 1;
        role->create_time_ = 0;
        role->is_last_ = false;
      }
    }
  }
  LOG_INFO << "delete role_id=" << role_id
           << " remaining=" << (role ? role->all_roles_.size() : 0);
}

SessionService::RoleLoginCheck SessionService::CheckRoleLogin(const EntityPtr& entity,
                                              uint64_t role_id) {
  auto cached = PlayerEntitySystem::Instance().FindByRoleId(role_id);
  if (cached && cached != entity) {
    auto* cached_acc = cached->GetComponent<AccountComponent>();
    auto* my_acc = entity->GetComponent<AccountComponent>();
    if (cached_acc && my_acc && cached_acc->uid_ != my_acc->uid_) {
      return RoleLoginCheck::kDeniedUid;
    }
    // 同账号顶角色：角色数据在 cached 上，允许登录
    return RoleLoginCheck::kOk;
  }
  auto* role = entity->GetComponent<RoleComponent>();
  if (!role) return RoleLoginCheck::kNotFound;
  for (const auto& info : role->all_roles_) {
    if (info.role_id == role_id) return RoleLoginCheck::kOk;
  }
  return RoleLoginCheck::kNotFound;
}

bool SessionService::OnRoleLogin(const EntityPtr& entity, uint64_t role_id) {
  if (CheckRoleLogin(entity, role_id) != RoleLoginCheck::kOk) {
    LOG_WARN << "role login denied by CheckRoleLogin role_id=" << role_id;
    return false;
  }
  auto cached = PlayerEntitySystem::Instance().FindByRoleId(role_id);
  if (cached && cached != entity) {
    entity->TransferGameplayComponentsFrom(cached);
    PlayerEntitySystem::Instance().CleanupEntity(cached);
  }

  if (!entity->HasComponent<RoleComponent>()) {
    entity->AddComponent<RoleComponent>();
  }
  auto* role = entity->GetComponent<RoleComponent>();
  bool found = false;
  for (const auto& info : role->all_roles_) {
    if (info.role_id == role_id) {
      role->MarkLast(role_id);
      role->SetCurrent(info);
      found = true;
      break;
    }
  }
  if (!found) {
    LOG_WARN << "role login denied: role_id=" << role_id
             << " not in all_roles (count=" << role->all_roles_.size() << ")";
    return false;
  }
  entity->SetState(Entity::State::kRoleSelected);
  PlayerEntitySystem::Instance().RegisterByRoleId(role_id, entity);

  LOG_INFO << "role login: role_id=" << role_id;
  return true;
}

std::string SessionService::OnRandomNameReq(uint32_t sex) {
  return random_name::Generate(sex);
}
