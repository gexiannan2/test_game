#include "handlers/move_handler.h"

#include <limits>

#include "client_3d.pb.h"
#include "client_common.pb.h"
#include "ecs/components/connection_component.h"
#include "ecs/components/map_component.h"
#include "ecs/components/role_component.h"
#include "ecs/components/transform_component.h"
#include "game_server.h"
#include "navigation/nav_system.h"
#include "protocol/pack_flags.h"
#include "server_constants.h"
#include "utils/entity_util.h"
#include "utils/map_util.h"
#include "utils/proto_fill.h"
#include "utils/transform_util.h"
#include "zrpc/base/logger.h"
#include "zrpc/net/tcp_connection.h"

namespace {

void SendMoveRes(GameServer* server, const ::zrpc::TcpConnectionPtr& conn,
                 const EntityPtr& entity, int32_t status, int32_t err_code) {
  auto* conn_comp = entity->GetComponent<ConnectionComponent>();
  if (!conn_comp || !conn || !conn->Connected()) {
    return;
  }
  auto* tfm = entity->GetComponent<TransformComponent>();
  auto* role = entity->GetComponent<RoleComponent>();
  ::cli_3d_move_res res;
  res.set_entity_id(role ? role->role_id_ : entity->GetId());
  proto_fill::FillMoveData(res.mutable_move(), tfm);
  res.mutable_move()->set_status(status);
  res.set_err_code(err_code);
  LOG_INFO << "[RES] cli_3d_move_res err_code=" << err_code
           << " status=" << status
           << " entity_id=" << (role ? role->role_id_ : 0);
  server->SendMsg(conn, proto_id("cli_3d_move_res"), res, &conn_comp->send_seq_);
}

bool IsAirLocomotionStatus(int32_t status) {
  return status == ::MOVE_STATUS_JUMP || status == ::MOVE_STATUS_FALL ||
         status == ::MOVE_STATUS_SLIDE;
}

int32_t NavErrorCode(game::navigation::NavStatus status) {
  using game::navigation::NavStatus;
  switch (status) {
    case NavStatus::navmesh_not_loaded:
    case NavStatus::heightmap_not_loaded:
    case NavStatus::layer_not_found:
      return server::kMoveErrNavUnavailable;
    case NavStatus::nearest_poly_not_found:
    case NavStatus::path_not_found:
    case NavStatus::out_of_range:
    case NavStatus::height_mismatch:
      return server::kMoveErrNavRejected;
    default:
      return server::kMoveErrFailed;
  }
}

}  // namespace

void MoveHandler::Handle(const ::zrpc::TcpConnectionPtr& conn,
                         const EntityPtr& entity, uint32_t msg_id,
                         const std::shared_ptr<::google::protobuf::Message>& req) {
  if (!game_util::RequireConn(entity, msg_id)) {
    return;
  }
  if (msg_id != proto_id("cli_3d_move_req")) {
    LOG_WARN << "MoveHandler unhandled msg_id=" << msg_id;
    return;
  }
  if (!game_util::RequireInGame(entity)) {
    SendMoveRes(server_, conn, entity, 0, server::kMoveErrFailed);
    return;
  }

  auto req_ptr = std::static_pointer_cast<::cli_3d_move_req>(req);
  if (!req_ptr) {
    SendMoveRes(server_, conn, entity, 0, server::kMoveErrFailed);
    return;
  }

  if (!req_ptr->has_move() || !req_ptr->move().has_pos()) {
    LOG_WARN << "move rejected: missing move or position entity="
             << entity->GetId();
    SendMoveRes(server_, conn, entity, 0, server::kMoveErrFailed);
    return;
  }

  const auto& m = req_ptr->move();
  JPH::Vec3 pos =
      m.has_pos() ? game_util::ExtractVec3(m.pos()) : JPH::Vec3::sZero();
  JPH::Quat rot =
      m.has_rot() ? game_util::ExtractQuat(m.rot()) : JPH::Quat::sIdentity();
  JPH::Quat move_rot =
      m.has_move_rot() ? game_util::ExtractQuat(m.move_rot()) : rot;
  JPH::Vec3 vel = m.has_velocity() ? game_util::ExtractVec3(m.velocity())
                                   : JPH::Vec3::sZero();

  LOG_INFO << "[REQ] cli_3d_move_req " << entity->LogTag()
           << " pos=" << game_util::FormatVec3(pos) << " status=" << m.status()
           << " rot=" << game_util::FormatQuat(rot)
           << " vel=" << game_util::FormatVec3(vel);

  // 空中位移状态必须走 jump 协议
  if (IsAirLocomotionStatus(m.status())) {
    LOG_WARN << "move rejected: status requires jump proto entity="
             << entity->GetId() << " status=" << m.status();
    SendMoveRes(server_, conn, entity, m.status(),
                server::kMoveErrUseJumpProto);
    return;
  }

  auto* tfm = entity->GetComponent<TransformComponent>();
  if (tfm && tfm->airborne_) {
    LOG_WARN << "move rejected: airborne, use jump proto entity="
             << entity->GetId() << " jump_type=" << tfm->jump_type_;
    SendMoveRes(server_, conn, entity, m.status(), server::kMoveErrAirborne);
    return;
  }

  if (!game_util::Vec3Finite(pos)) {
    SendMoveRes(server_, conn, entity, m.status(), server::kMoveErrFailed);
    return;
  }
  if (!game_util::QuatFinite(rot) || !game_util::QuatFinite(move_rot) ||
      !game_util::Vec3Finite(vel)) {
    SendMoveRes(server_, conn, entity, m.status(), server::kMoveErrFailed);
    return;
  }

  if (tfm == nullptr) {
    LOG_ERROR << "move rejected: TransformComponent missing entity="
              << entity->GetId();
    SendMoveRes(server_, conn, entity, m.status(), server::kMoveErrFailed);
    return;
  }

  if (server_->NavigationEnabled()) {
    const auto* map = entity->GetComponent<MapComponent>();
    auto* navigation = server_->GetNavSystem();
    if (!map || map->map_cfg_id_ == 0 ||
        map->map_cfg_id_ > std::numeric_limits<uint32_t>::max() ||
        navigation == nullptr) {
      LOG_ERROR << "move rejected: navigation context unavailable entity="
                << entity->GetId();
      SendMoveRes(server_, conn, entity, m.status(),
                  server::kMoveErrNavUnavailable);
      return;
    }

    const auto map_id = static_cast<game::navigation::map_id_t>(
        map->map_cfg_id_);
    const game::navigation::NavPosition from{
        tfm->pos_.GetX(), tfm->pos_.GetY(), tfm->pos_.GetZ()};
    game::navigation::NavPosition target{
        pos.GetX(), from.y, pos.GetZ()};

    game::navigation::NavStatus nav_status =
        game::navigation::NavStatus::query_failed;
    if (server_->HeightMapValidationEnabled()) {
      auto* height_maps = server_->GetHeightMapSystem();
      if (height_maps == nullptr) {
        nav_status = game::navigation::NavStatus::heightmap_not_loaded;
      } else {
        target.y = pos.GetY();
        const auto validation = navigation->validate_player_move(
            *height_maps,
            map_id,
            from,
            target,
            tfm->nav_height_layer_,
            from.y);
        nav_status = validation.status;
        if (nav_status == game::navigation::NavStatus::success) {
          pos.SetY(validation.server_position.y);
          tfm->nav_height_layer_ = validation.height_layer;
        }
      }
    } else {
      // 当前仅检查 XZ 与直线可通行性，客户端 Y 暂沿用原有流程。
      nav_status = navigation
                       ->validate_direct_move(
                           map_id,
                           from,
                           target,
                           server_->NavmeshMaxHorizontalError())
                       .status;
    }

    if (nav_status != game::navigation::NavStatus::success) {
      LOG_WARN << "move rejected by navigation entity=" << entity->GetId()
               << " map_cfg_id=" << map_id
               << " status=" << game::navigation::to_string(nav_status)
               << " from=" << game_util::FormatVec3(tfm->pos_)
               << " target=" << game_util::FormatVec3(pos);
      SendMoveRes(server_, conn, entity, m.status(), NavErrorCode(nav_status));
      return;
    }
  }

  server::ClampToMap(server_->GetJoltServer(), pos);

  entity->SetMoveState(rot, move_rot, vel, EntityPropertyType::kMove);
  server_->GetWorld()->MoveEntity(entity, pos, EntityPropertyType::kMove);
  LOG_INFO << "[MOVE] " << entity->LogTag()
           << " applied_pos=" << game_util::FormatVec3(pos)
           << " rot=" << game_util::FormatQuat(rot);
  SendMoveRes(server_, conn, entity, m.status(), server::kMoveErrOk);
}
