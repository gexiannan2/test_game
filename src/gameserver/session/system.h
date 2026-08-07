#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <Jolt/Jolt.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Quat.h>

#include "ecs/components/account_component.h"
#include "ecs/components/role_component.h"
#include "ecs/entity/entity.h"
#include "ecs/systems/map_config_system.h"
#include "ecs/systems/player_entity_system.h"

// Session 层门面：连接 / 账号 / 角色（与 ECS WorldSystem 等仿真系统分离）
class SessionService {
 public:
  static void OnHandshake(const EntityPtr& entity, const std::string& version);
  static void OnHeartbeat(const EntityPtr& entity);
  static void OnReconnect(const EntityPtr& entity, uint64_t session_id);
  static void OnKickoff(const EntityPtr& entity, uint64_t role_id,
                        uint32_t code_id, const std::string& reason);
  static uint64_t OnUserLogin(const EntityPtr& entity,
                              const std::string& uid, const std::string& token,
                              uint32_t channel_id, uint64_t session_id);
  static void OnRoleListReq(const EntityPtr& entity);
  static void OnRoleCreate(const EntityPtr& entity, uint64_t role_id,
                           const std::string& name, uint32_t sex, uint32_t job);
  static void OnRoleDelete(const EntityPtr& entity, uint64_t role_id);
  // 选角前校验（不改状态）：Ok / DeniedUid / NotFound
  enum class RoleLoginCheck { kOk, kDeniedUid, kNotFound };
  static RoleLoginCheck CheckRoleLogin(const EntityPtr& entity, uint64_t role_id);
  static bool OnRoleLogin(const EntityPtr& entity, uint64_t role_id);
  static std::string OnRandomNameReq(uint32_t sex);
};
