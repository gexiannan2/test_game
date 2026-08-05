#include "handlers/game_handler.h"

#include "client_3d.pb.h"
#include "client_login.pb.h"
#include "ecs/components/account_component.h"
#include "ecs/components/connection_component.h"
#include "ecs/components/map_component.h"
#include "ecs/components/role_component.h"
#include "ecs/components/transform_component.h"
#include "ecs/entity/entity.h"
#include "session/system.h"
#include "game_server.h"
#include "protocol/pack_flags.h"
#include "server_constants.h"
#include "zrpc/base/logger.h"
#include "zrpc/net/tcp_connection.h"

void GameHandler::Handle(const ::zrpc::TcpConnectionPtr& conn,
                          const EntityPtr& entity,
                          uint32_t msg_id,
                          const std::shared_ptr<::google::protobuf::Message>& req) {
  (void)req;
  auto* conn_comp = entity->GetComponent<ConnectionComponent>();
  if (!conn_comp) {
    LOG_WARN << "no ConnectionComponent, drop msg_id=" << msg_id;
    return;
  }
  if (msg_id != proto_id("cli_enter_game_req")) {
    LOG_WARN << "GameHandler unhandled msg_id=" << msg_id;
    return;
  }

  auto send_enter_err = [&](int32_t err) {
    ::cli_enter_game_res res;
    res.set_err_code(err);
    server_->SendMsg(conn, proto_id("cli_enter_game_res"), res, &conn_comp->send_seq_);
  };

  if (entity->GetState() == Entity::State::kInGame && entity->IsInMap()) {
    LOG_WARN << "enter game: already in game";
    send_enter_err(server::kEnterGameErrAlreadyInMap);
    return;
  }

  const bool can_enter =
      entity->GetState() == Entity::State::kRoleSelected ||
      (entity->GetState() == Entity::State::kInGame && !entity->IsInMap());
  if (!can_enter) {
    LOG_WARN << "enter game without role selected, state="
             << static_cast<int>(entity->GetState());
    send_enter_err(server::kEnterGameErrBadState);
    return;
  }

  auto* role = entity->GetComponent<RoleComponent>();
  if (!role || role->role_id_ == 0) {
    LOG_WARN << "enter game: no role selected";
    send_enter_err(server::kEnterGameErrNoRole);
    return;
  }

  LOG_INFO << "[REQ] cli_enter_game_req";

  if (!conn_comp->conn_ || !conn_comp->conn_->Connected()) {
    LOG_WARN << "enter game: connection lost";
    send_enter_err(server::kEnterGameErrConnLost);
    return;
  }

  auto* default_map = MapConfigSystem::Instance().GetFirstMap();
  if (!default_map) {
    LOG_WARN << "enter game: no map config available, refuse spawn";
    send_enter_err(server::kEnterGameErrNoMap);
    return;
  }

  // 有 Transform 不代表进过图，用 MapComponent 判首次进图
  const bool first_enter = !entity->HasComponent<MapComponent>();
  if (!entity->HasComponent<TransformComponent>()) {
    entity->AddComponent<TransformComponent>();
  }
  auto* tfm = entity->GetComponent<TransformComponent>();
  if (first_enter) {
    tfm->pos_ = default_map->born_pos_;
    tfm->rot_ = default_map->born_rot_;
    tfm->move_rot_ = default_map->born_move_rot_;
    LOG_INFO << "[SPAWN] " << entity->LogTag()
             << " pos=(" << tfm->pos_.GetX() << "," << tfm->pos_.GetY()
             << "," << tfm->pos_.GetZ() << ")"
             << " rot=(" << tfm->rot_.GetX() << "," << tfm->rot_.GetY()
             << "," << tfm->rot_.GetZ() << "," << tfm->rot_.GetW() << ")";
    entity->AddComponent<MapComponent>();
    entity->GetComponent<MapComponent>()->map_cfg_id_ = default_map->cfg_id_;
    entity->GetComponent<MapComponent>()->map_ins_id_ =
        server::kDefaultMapInstanceId;
  }

  // 与 reconnect 一致：残留 IsInMap 时 EnterMap 会短路，须先离图
  if (entity->IsInMap()) {
    server_->GetWorld()->LeaveMap(entity);
  }

  entity->SetState(Entity::State::kInGame);

  ::cli_enter_game_res res;
  res.set_err_code(server::kEnterGameErrOk);
  LOG_INFO << "[RES] cli_enter_game_res err_code=" << res.err_code();
  server_->SendMsg(conn, proto_id("cli_enter_game_res"), res, &conn_comp->send_seq_);

  ::cli_global_config_ntf cfg;
  LOG_INFO << "[RES] cli_global_config_ntf (empty body)";
  server_->SendMsg(conn, proto_id("cli_global_config_ntf"), cfg, &conn_comp->send_seq_);

  server_->GetWorld()->EnterMap(entity);

  LOG_INFO << "entered game: AOI visible_count="
           << server_->GetWorld()->GetVisibleEntities(entity->GetId()).size();
}
