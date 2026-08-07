// 账号首次登录（同 uid 顶号在 account_handler 处理）。

#include "session/system.h"

#include "ecs/components/account_component.h"
#include "ecs/components/role_component.h"
#include "ecs/entity/entity.h"
#include "zrpc/base/logger.h"

uint64_t SessionService::OnUserLogin(const EntityPtr& entity,
                                     const std::string& uid,
                                     const std::string& token,
                                     uint32_t channel_id,
                                     uint64_t session_id) {
  LOG_INFO << "user login (first/new): uid=" << uid;

  if (!entity->HasComponent<AccountComponent>()) {
    entity->AddComponent<AccountComponent>();
  }

  auto* acc = entity->GetComponent<AccountComponent>();
  acc->uid_        = uid;
  acc->token_      = token;
  acc->channel_id_ = channel_id;
  // 同实体再登录须失效旧 session，否则旧 session 仍可重连
  const uint64_t old_session_id = acc->session_id_;
  acc->session_id_ = session_id;
  entity->SetState(Entity::State::kLoggedIn);

  PlayerEntitySystem::Instance().RegisterByUid(uid, entity);
  if (old_session_id != 0 && old_session_id != session_id) {
    PlayerEntitySystem::Instance().UnregisterBySessionId(old_session_id);
  }
  PlayerEntitySystem::Instance().RegisterBySessionId(session_id, entity);

  auto* role = entity->GetComponent<RoleComponent>();
  if (role && role->role_id_ != 0) {
    PlayerEntitySystem::Instance().RegisterByRoleId(role->role_id_, entity);
  }

  LOG_INFO << "user login: uid=" << uid
           << " channel=" << channel_id
           << " session_id=" << session_id
           << " role_id=" << (role ? role->role_id_ : 0)
           << " cached_players="
           << PlayerEntitySystem::Instance().GetPlayerCount();
  return session_id;
}
