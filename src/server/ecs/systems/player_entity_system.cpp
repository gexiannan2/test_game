// PlayerEntitySystem 索引维护。

#include "ecs/systems/player_entity_system.h"

#include "ecs/components/account_component.h"
#include "ecs/components/role_component.h"

void PlayerEntitySystem::Unregister(uint64_t role_id) {
  std::lock_guard<std::mutex> lk(mutex_);
  role_id_map_.erase(role_id);
}

void PlayerEntitySystem::CleanupEntity(const EntityPtr& old_entity) {
  if (!old_entity) return;
  std::lock_guard<std::mutex> lk(mutex_);
  auto* acc = old_entity->GetComponent<AccountComponent>();
  auto* role = old_entity->GetComponent<RoleComponent>();
  // 仅当索引仍指向本实体时才擦除，避免顶号后误删新连接已占用的 uid/session
  if (acc) {
    auto uit = uid_map_.find(acc->uid_);
    if (uit != uid_map_.end() && uit->second == old_entity) {
      uid_map_.erase(uit);
    }
    auto sit = session_id_map_.find(acc->session_id_);
    if (sit != session_id_map_.end() && sit->second == old_entity) {
      session_id_map_.erase(sit);
    }
  }
  if (role && role->role_id_ != 0) {
    auto it = role_id_map_.find(role->role_id_);
    if (it != role_id_map_.end() && it->second == old_entity) {
      role_id_map_.erase(it);
    }
  }
}
