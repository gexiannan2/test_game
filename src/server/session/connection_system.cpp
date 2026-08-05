// 连接层：握手、心跳、重连、踢下线。

#include "session/system.h"

#include <ctime>

#include "ecs/components/connection_component.h"
#include "ecs/entity/entity.h"
#include "zrpc/base/logger.h"

void System::OnHandshake(const EntityPtr& entity,
                                    const std::string& version) {
  LOG_INFO << "handshake from: " << version;
  entity->SetState(Entity::State::kHandshaked);

  auto* conn = entity->GetComponent<ConnectionComponent>();
  if (conn) {
    conn->last_heartbeat_sec_ = static_cast<uint64_t>(std::time(nullptr));
  }
}

void System::OnHeartbeat(const EntityPtr& entity) {
  auto* conn = entity->GetComponent<ConnectionComponent>();
  if (conn) {
    conn->last_heartbeat_sec_ = static_cast<uint64_t>(std::time(nullptr));
  }
  // LOG_INFO << "heartbeat from entity=" << entity->GetId();
}

void System::OnReconnect(const EntityPtr& entity,
                                    uint64_t session_id) {
  LOG_INFO << "reconnect session_id=" << session_id;
  entity->SetState(Entity::State::kLoggedIn);
  auto* conn = entity->GetComponent<ConnectionComponent>();
  if (conn) {
    conn->last_heartbeat_sec_ = static_cast<uint64_t>(std::time(nullptr));
  }
}

void System::OnKickoff(const EntityPtr& entity, uint64_t role_id,
                                  uint32_t code_id,
                                  const std::string& reason) {
  LOG_INFO << "kickoff role_id=" << role_id
           << " code=" << code_id << " reason=" << reason;
  // 离图须由调用方先 LeaveMap；此处只改连接态，避免 SetInMap(false) 留下 AOI 幽灵
  if (entity->IsInMap()) {
    LOG_WARN << "OnKickoff while still in map entity=" << entity->GetId()
             << " — caller should LeaveMap first";
  }
  entity->SetState(Entity::State::kDisconnected);
}
