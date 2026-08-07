// GameServer 连接生命周期：建连/断连、踢线、心跳超时、优雅停服

#include "game_server.h"

#include <any>
#include <ctime>
#include <vector>

#include "client_login.pb.h"
#include "ecs/components/connection_component.h"
#include "ecs/components/role_component.h"
#include "ecs/entity/player_entity.h"
#include "ecs/systems/player_entity_system.h"
#include "protocol/pack_flags.h"
#include "server_constants.h"
#include "zrpc/base/logger.h"
#include "zrpc/net/socket.h"
#include "zrpc/net/tcp_connection.h"

void GameServer::DoGracefulStop() {
  LOG_INFO << "graceful shutdown requested, draining all connections...";
  if (heartbeat_timer_) {
    loop_.CancelAfter(heartbeat_timer_);
    heartbeat_timer_.reset();
  }
  if (world_tick_timer_) {
    loop_.CancelAfter(world_tick_timer_);
    world_tick_timer_.reset();
  }
  if (persist_timer_) {
    loop_.CancelAfter(persist_timer_);
    persist_timer_.reset();
  }
  if (mongo_ping_timer_) {
    loop_.CancelAfter(mongo_ping_timer_);
    mongo_ping_timer_.reset();
  }

  if (world_) {
    const auto all_players = PlayerEntitySystem::Instance().GetAllByUidSnapshot();
    for (const auto& [uid, entity] : all_players) {
      (void)uid;
      if (entity && entity->IsInMap()) {
        world_->LeaveMap(entity);
      }
    }
  }
  if (player_persist_) {
    player_persist_->FlushOnShutdown();
  }
  server_.Stop([weak = weak_from_this()]() {
    auto self = weak.lock();
    if (!self) {
      return;
    }
    LOG_INFO << "all connections drained, tcp server stopped, quit loop";
    self->loop_.Quit();
  });
}

void GameServer::OnConnection(const ::zrpc::TcpConnectionPtr& conn) {
  if (conn->Connected()) {
    ::zrpc::socket::SetKeepAlive(conn->GetSockfd(), 1);
    auto player = std::make_shared<PlayerEntity>(world_->AllocateEntityId());
    player->AddComponent<ConnectionComponent>();
    player->GetComponent<ConnectionComponent>()->conn_ = conn;
    conn->SetContext(EntityPtr(player));
    LOG_INFO << "client connected, player=" << player->GetId();
  } else {
    auto* any_ptr = std::any_cast<EntityPtr>(&conn->GetContext());
    if (!any_ptr || !(*any_ptr)) {
      LOG_WARN << "disconnect with no entity context";
      return;
    }
    auto entity = *any_ptr;

    auto* cc = entity->GetComponent<ConnectionComponent>();
    if (cc && cc->conn_ && cc->conn_ != conn) {
      LOG_INFO << "stale disconnect ignored, entity=" << entity->GetId()
               << " owned_by_other_conn";
      return;
    }

    entity->SetState(Entity::State::kDisconnected);

    if (cc) {
      cc->conn_.reset();
    }

    if (entity->IsInMap()) {
      world_->LeaveMap(entity);
    }
    entity->RemoveComponent<ConnectionComponent>();
    if (player_persist_) {
      auto* role = entity->GetComponent<RoleComponent>();
      if (role && role->role_id_ != 0) {
        player_persist_->FlushPlayer(role->role_id_);
      }
    }
    LOG_INFO << "client disconnected, entity cached: id=" << entity->GetId()
             << " cached_players="
             << PlayerEntitySystem::Instance().GetPlayerCount();
  }
}

void GameServer::KickAndShutdownConnection(
    const EntityPtr& entity, uint32_t code_id, const char* reason,
    const ::zrpc::TcpConnectionPtr& except_conn) {
  if (!entity) {
    return;
  }
  auto* old_conn_comp = entity->GetComponent<ConnectionComponent>();
  if (!old_conn_comp || !old_conn_comp->conn_ ||
      !old_conn_comp->conn_->Connected()) {
    return;
  }
  if (except_conn && old_conn_comp->conn_ == except_conn) {
    return;
  }
  ::cli_kickoff_player_ntf kickoff;
  auto* old_role = entity->GetComponent<RoleComponent>();
  kickoff.set_role_id(old_role ? old_role->role_id_ : 0);
  kickoff.set_code_id(code_id);
  kickoff.set_reason(reason ? reason : "");
  SendMsg(old_conn_comp->conn_, proto_id("cli_kickoff_player_ntf"), kickoff,
          &old_conn_comp->send_seq_);
  if (player_persist_ && old_role && old_role->role_id_ != 0) {
    player_persist_->FlushPlayer(old_role->role_id_);
  }
  old_conn_comp->conn_->SetContext(EntityPtr{});
  old_conn_comp->conn_->Shutdown();
  LOG_INFO << "kickoff connection entity=" << entity->GetId()
           << " code=" << code_id << " reason=" << (reason ? reason : "");
}

void GameServer::CheckHeartbeatTimeout() {
  uint64_t now = static_cast<uint64_t>(std::time(nullptr));
  std::vector<EntityPtr> timeout_entities;

  const auto all_players = PlayerEntitySystem::Instance().GetAllByUidSnapshot();
  for (const auto& [uid, entity] : all_players) {
    (void)uid;
    auto* conn = entity->GetComponent<ConnectionComponent>();
    if (!conn || !conn->conn_) {
      continue;
    }
    if (entity->GetState() == Entity::State::kConnected ||
        entity->GetState() == Entity::State::kDisconnected) {
      continue;
    }
    if (conn->last_heartbeat_sec_ > 0 &&
        (now - conn->last_heartbeat_sec_) >
            static_cast<uint64_t>(HeartbeatTimeoutSec())) {
      timeout_entities.push_back(entity);
    }
  }

  for (const auto& entity : timeout_entities) {
    auto* conn = entity->GetComponent<ConnectionComponent>();
    auto* role = entity->GetComponent<RoleComponent>();
    LOG_WARN << "heartbeat timeout, kicking entity=" << entity->GetId()
             << " role_id=" << (role ? role->role_id_ : 0)
             << " last_heartbeat=" << conn->last_heartbeat_sec_
             << " now=" << now;

    if (entity->IsInMap()) {
      world_->LeaveMap(entity);
    }

    entity->SetState(Entity::State::kDisconnected);

    ::zrpc::TcpConnectionPtr conn_ptr;
    if (conn && conn->conn_) {
      conn_ptr = conn->conn_;
      conn_ptr->SetContext(EntityPtr{});
    }

    entity->RemoveComponent<ConnectionComponent>();

    if (player_persist_) {
      if (role && role->role_id_ != 0) {
        player_persist_->FlushPlayer(role->role_id_);
      }
    }

    if (conn_ptr) {
      conn_ptr->Shutdown();
    }
  }

  if (!timeout_entities.empty()) {
    LOG_INFO << "heartbeat check: kicked " << timeout_entities.size()
             << " timeout players";
  }
}
