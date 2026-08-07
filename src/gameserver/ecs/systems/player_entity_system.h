#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <utility>

#include "ecs/entity/entity.h"
#include "common/player_config.h"

// 玩家实体全局索引：断线不删实体，支持顶号/重连。
class PlayerEntitySystem {
 public:
  static PlayerEntitySystem& Instance() {
    static PlayerEntitySystem inst;
    return inst;
  }

  void RegisterByRoleId(uint64_t role_id, const EntityPtr& entity) {
    std::lock_guard<std::mutex> lk(mutex_);
    role_id_map_[role_id] = entity;
  }
  EntityPtr FindByRoleId(uint64_t role_id) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = role_id_map_.find(role_id);
    return (it != role_id_map_.end()) ? it->second : nullptr;
  }

  void RegisterBySessionId(uint64_t session_id, const EntityPtr& entity) {
    std::lock_guard<std::mutex> lk(mutex_);
    session_id_map_[session_id] = entity;
  }
  void UnregisterBySessionId(uint64_t session_id) {
    std::lock_guard<std::mutex> lk(mutex_);
    session_id_map_.erase(session_id);
  }
  EntityPtr FindBySessionId(uint64_t session_id) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = session_id_map_.find(session_id);
    return (it != session_id_map_.end()) ? it->second : nullptr;
  }

  void RegisterByUid(const std::string& uid, const EntityPtr& entity) {
    std::lock_guard<std::mutex> lk(mutex_);
    uid_map_[uid] = entity;
  }
  EntityPtr FindByUid(const std::string& uid) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = uid_map_.find(uid);
    return (it != uid_map_.end()) ? it->second : nullptr;
  }

  void Unregister(uint64_t role_id);
  void CleanupEntity(const EntityPtr& old_entity);

  std::vector<std::pair<uint64_t, EntityPtr>> GetAllByRoleIdSnapshot() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return {role_id_map_.begin(), role_id_map_.end()};
  }
  std::vector<std::pair<std::string, EntityPtr>> GetAllByUidSnapshot() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return {uid_map_.begin(), uid_map_.end()};
  }
  size_t GetPlayerCount() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return role_id_map_.size();
  }

 private:
  PlayerEntitySystem() = default;
  mutable std::mutex mutex_;
  std::unordered_map<uint64_t, EntityPtr> role_id_map_;
  std::unordered_map<uint64_t, EntityPtr> session_id_map_;
  std::unordered_map<std::string, EntityPtr> uid_map_;
};
