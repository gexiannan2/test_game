#include "session/session_restore.h"

#include <ctime>

#include "ecs/components/connection_component.h"
#include "ecs/components/player_data_component.h"
#include "ecs/systems/player_entity_system.h"
#include "ecs/systems/world_system.h"
#include "game_server.h"
#include "zrpc/base/logger.h"
#include "zrpc/net/tcp_connection.h"

void RebindCachedEntity(GameServer* server,
                        const EntityPtr& cached,
                        const EntityPtr& temp_entity,
                        const ::zrpc::TcpConnectionPtr& conn,
                        uint32_t kick_code,
                        const char* kick_reason,
                        const SessionRestoreOptions& opts,
                        SessionRestoreMidFn mid) {
  if (!server || !cached || !conn) {
    return;
  }

  if (opts.leave_map_before_kick && cached->IsInMap()) {
    server->GetWorld()->LeaveMap(cached);
  }

  server->KickAndShutdownConnection(cached, kick_code, kick_reason, conn);

  if (!cached->HasComponent<ConnectionComponent>()) {
    cached->AddComponent<ConnectionComponent>();
  }
  auto* bind_cc = cached->GetComponent<ConnectionComponent>();
  bind_cc->conn_ = conn;
  bind_cc->last_heartbeat_sec_ = static_cast<uint64_t>(std::time(nullptr));

  const bool was_loading =
      cached->GetState() == Entity::State::kDataLoading;
  if (opts.clear_data_loading && was_loading) {
    cached->RemoveComponent<PlayerDataComponent>();
    LOG_INFO << "session_restore: cleanup kDataLoading残留 entity="
             << cached->GetId();
  }

  const bool was_in_game =
      !was_loading &&
      (cached->GetState() == Entity::State::kInGame || cached->IsInMap());

  if (opts.leave_map_after_bind && cached->IsInMap()) {
    server->GetWorld()->LeaveMap(cached);
  }

  if (opts.apply_state_after) {
    cached->SetState(opts.state_after);
  }

  if (mid) {
    mid(cached, was_in_game);
  }

  if (opts.reenter_map_if_was_in_game && was_in_game) {
    LOG_INFO << "session_restore: restoring game state, entity="
             << cached->GetId();
    if (cached->IsInMap()) {
      server->GetWorld()->LeaveMap(cached);
    }
    cached->SetState(Entity::State::kInGame);
    server->GetWorld()->EnterMap(cached);
  }

  if (temp_entity && temp_entity != cached) {
    PlayerEntitySystem::Instance().CleanupEntity(temp_entity);
  }
  conn->SetContext(EntityPtr(cached));
}
