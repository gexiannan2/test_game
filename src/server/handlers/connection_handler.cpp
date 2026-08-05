#include "handlers/connection_handler.h"

#include <ctime>

#include "client_login.pb.h"
#include "ecs/components/account_component.h"
#include "ecs/components/connection_component.h"
#include "ecs/components/role_component.h"
#include "ecs/entity/entity.h"
#include "session/system.h"
#include "game_server.h"
#include "protocol/pack_flags.h"
#include "server_constants.h"
#include "zrpc/base/logger.h"
#include "zrpc/net/tcp_connection.h"

void ConnectionHandler::Handle(const ::zrpc::TcpConnectionPtr& conn,
                          const EntityPtr& entity,
                          uint32_t msg_id,
                          const std::shared_ptr<::google::protobuf::Message>& req) {
  if (!entity->GetComponent<ConnectionComponent>()) {
    LOG_WARN << "no ConnectionComponent, drop msg_id=" << msg_id;
    return;
  }
  if (msg_id == proto_id("cli_handshake_req")) {
      auto req_ptr = std::static_pointer_cast<::cli_handshake_req>(req);
      if (!req_ptr) {
        LOG_WARN << "connection_handler static_pointer_cast<cli_handshake_req> failed";
        return;
      }
      LOG_INFO << "[REQ] cli_handshake_req version=" << req_ptr->version();
      System::OnHandshake(entity, req_ptr->version());
      ::cli_handshake_res res;
      res.set_err_code(0);
      res.set_msg("svc_game_3d_server/1.0");
      LOG_INFO << "[RES] cli_handshake_res err_code=" << res.err_code()
               << " msg=" << res.msg();
      server_->SendMsg(conn, proto_id("cli_handshake_res"), res,
                       &entity->GetComponent<ConnectionComponent>()->send_seq_);
  } else if (msg_id == proto_id("cli_heart_beat_req")) {
      System::OnHeartbeat(entity);
      ::cli_heart_beat_res res;
      res.set_err_code(0);
      res.set_server_time(static_cast<uint64_t>(std::time(nullptr)));
      server_->SendMsg(conn, proto_id("cli_heart_beat_res"), res,
                       &entity->GetComponent<ConnectionComponent>()->send_seq_);
  } else if (msg_id == proto_id("cli_reconnect_req")) {
      auto req_ptr = std::static_pointer_cast<::cli_reconnect_req>(req);
      if (!req_ptr) {
        LOG_WARN << "connection_handler static_pointer_cast<cli_reconnect_req> failed";
        return;
      }
      LOG_INFO << "[REQ] cli_reconnect_req session_id=" << req_ptr->session_id();
      auto cached = PlayerEntitySystem::Instance().FindBySessionId(
          req_ptr->session_id());
      auto* cached_acc =
          cached ? cached->GetComponent<AccountComponent>() : nullptr;
      if (!cached || !cached_acc ||
          cached_acc->session_id_ != req_ptr->session_id()) {
        LOG_WARN << "reconnect with invalid session_id="
                 << req_ptr->session_id();
        ::cli_reconnect_res res;
        res.set_session_id(req_ptr->session_id());
        res.set_err_code(1);
        server_->SendMsg(conn, proto_id("cli_reconnect_res"), res,
                         &entity->GetComponent<ConnectionComponent>()
                              ->send_seq_);
        return;
      }

      server_->KickAndShutdownConnection(
          cached, server::kKickoffReplaceAccount, "reconnect", conn);

      LOG_INFO << "reconnect: replacing temp entity=" << entity->GetId()
               << " with cached entity=" << cached->GetId();

      if (!cached->HasComponent<ConnectionComponent>()) {
        cached->AddComponent<ConnectionComponent>();
      }
      auto* bind_cc = cached->GetComponent<ConnectionComponent>();
      bind_cc->conn_ = conn;
      bind_cc->last_heartbeat_sec_ =
          static_cast<uint64_t>(std::time(nullptr));

      // 用状态/在图标志判断，勿用 HasComponent<MapComponent>（enter_game 中途
      // 失败也可能留下组件，造成误恢复进图）。
      const bool was_in_game =
          cached->GetState() == Entity::State::kInGame || cached->IsInMap();

      System::OnReconnect(cached, req_ptr->session_id());

      ::cli_reconnect_res res;
      res.set_session_id(req_ptr->session_id());
      res.set_err_code(0);
      server_->SendMsg(conn, proto_id("cli_reconnect_res"), res,
                       &cached->GetComponent<ConnectionComponent>()->send_seq_);

      if (was_in_game) {
        LOG_INFO << "reconnect restoring game state, entity="
                 << cached->GetId();
        // IsInMap 时 EnterMap 会短路；须先 LeaveMap 再重建 AOI/足迹
        if (cached->IsInMap()) {
          server_->GetWorld()->LeaveMap(cached);
        }
        cached->SetState(Entity::State::kInGame);
        server_->GetWorld()->EnterMap(cached);
      }

      PlayerEntitySystem::Instance().CleanupEntity(entity);
      conn->SetContext(cached);
  } else {
      LOG_WARN << "ConnectionHandler unhandled msg_id=" << msg_id;
  }
}
