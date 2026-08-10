#include "handlers/move_handler.h"

#include <cmath>

#include "client_3d.pb.h"
#include "client_common.pb.h"
#include "ecs/components/connection_component.h"
#include "ecs/components/role_component.h"
#include "ecs/components/transform_component.h"
#include "ecs/entity/entity.h"
#include "game_server.h"
#include "map_bounds_util.h"
#include "protocol/pack_flags.h"
#include "server_constants.h"
#include "zrpc/base/logger.h"
#include "zrpc/net/tcp_connection.h"

namespace {

// 填充 entity_move_data（嵌套在 cli_3d_move_res.move 中）
void FillMoveData(::entity_move_data* m, const TransformComponent* tfm) {
  if (!m || !tfm) return;
  auto* p = m->mutable_pos();
  p->set_x(tfm->pos_.GetX());
  p->set_y(tfm->pos_.GetY());
  p->set_z(tfm->pos_.GetZ());
  auto* r = m->mutable_rot();
  r->set_x(tfm->rot_.GetX());
  r->set_y(tfm->rot_.GetY());
  r->set_z(tfm->rot_.GetZ());
  r->set_w(tfm->rot_.GetW());
  auto* mr = m->mutable_move_rot();
  mr->set_x(tfm->move_rot_.GetX());
  mr->set_y(tfm->move_rot_.GetY());
  mr->set_z(tfm->move_rot_.GetZ());
  mr->set_w(tfm->move_rot_.GetW());
  auto* v = m->mutable_velocity();
  v->set_x(tfm->velocity_.GetX());
  v->set_y(tfm->velocity_.GetY());
  v->set_z(tfm->velocity_.GetZ());
}

void SendMoveRes(GameServer* server, const ::zrpc::TcpConnectionPtr& conn,
                 const EntityPtr& entity, int32_t status, bool success) {
  auto* conn_comp = entity->GetComponent<ConnectionComponent>();
  if (!conn_comp || !conn || !conn->Connected()) return;
  auto* tfm = entity->GetComponent<TransformComponent>();
  auto* role = entity->GetComponent<RoleComponent>();
  ::cli_3d_move_res res;
  res.set_entity_id(role ? role->role_id_ : entity->GetId());
  FillMoveData(res.mutable_move(), tfm);
  res.mutable_move()->set_status(status);
  res.set_err_code(success ? 0 : 1);
  LOG_INFO << "[RES] cli_3d_move_res err_code=" << res.err_code()
           << " status=" << status
           << " entity_id=" << (role ? role->role_id_ : 0);
  server->SendMsg(conn, proto_id("cli_3d_move_res"), res, &conn_comp->send_seq_);
}

bool QuatFinite(const JPH::Quat& q) {
  return std::isfinite(q.GetX()) && std::isfinite(q.GetY()) &&
         std::isfinite(q.GetZ()) && std::isfinite(q.GetW());
}

}  // namespace

void MoveHandler::Handle(const ::zrpc::TcpConnectionPtr& conn,
                          const EntityPtr& entity,
                          uint32_t msg_id,
                          const std::shared_ptr<::google::protobuf::Message>& req) {
  if (!entity->GetComponent<ConnectionComponent>()) {
    LOG_WARN << "no ConnectionComponent, drop msg_id=" << msg_id;
    return;
  }
  if (msg_id != proto_id("cli_3d_move_req")) {
    LOG_WARN << "MoveHandler unhandled msg_id=" << msg_id;
    return;
  }

  if (!entity->IsInMap() || entity->GetState() != Entity::State::kInGame) {
    LOG_WARN << "move req but not in game, entity=" << entity->GetId();
    SendMoveRes(server_, conn, entity, 0, false);
    return;
  }

  auto req_ptr = std::static_pointer_cast<::cli_3d_move_req>(req);
  if (!req_ptr) {
    LOG_WARN << "move_handler static_pointer_cast<cli_3d_move_req> failed";
    SendMoveRes(server_, conn, entity, 0, false);
    return;
  }
  const auto& m = req_ptr->move();
  LOG_INFO << "[REQ] cli_3d_move_req " << entity->LogTag()
           << " pos=(" << m.pos().x() << "," << m.pos().y() << "," << m.pos().z() << ")"
           << " status=" << m.status()
           << " rot=(" << m.rot().x() << "," << m.rot().y() << ","
           << m.rot().z() << "," << m.rot().w() << ")"
           << " vel=(" << (m.has_velocity() ? m.velocity().x() : 0) << ","
           << (m.has_velocity() ? m.velocity().y() : 0) << ","
           << (m.has_velocity() ? m.velocity().z() : 0) << ")";

  JPH::Vec3 pos(m.has_pos()
                    ? JPH::Vec3(m.pos().x(), m.pos().y(), m.pos().z())
                    : JPH::Vec3::sZero());
  if (!std::isfinite(pos.GetX()) || !std::isfinite(pos.GetY()) ||
      !std::isfinite(pos.GetZ())) {
    LOG_WARN << "move req rejected: non-finite pos entity=" << entity->GetId();
    SendMoveRes(server_, conn, entity, m.status(), false);
    return;
  }
  JPH::Quat rot(m.has_rot()
                    ? JPH::Quat(m.rot().x(), m.rot().y(),
                                m.rot().z(), m.rot().w())
                    : JPH::Quat::sIdentity());
  JPH::Quat move_rot(
      m.has_move_rot()
          ? JPH::Quat(m.move_rot().x(), m.move_rot().y(),
                      m.move_rot().z(), m.move_rot().w())
          : rot);
  JPH::Vec3 vel(m.has_velocity()
                    ? JPH::Vec3(m.velocity().x(), m.velocity().y(),
                                m.velocity().z())
                    : JPH::Vec3::sZero());
  if (!QuatFinite(rot) || !QuatFinite(move_rot) || !std::isfinite(vel.GetX()) ||
      !std::isfinite(vel.GetY()) || !std::isfinite(vel.GetZ())) {
    LOG_WARN << "move req rejected: non-finite rot/velocity entity="
             << entity->GetId();
    SendMoveRes(server_, conn, entity, m.status(), false);
    return;
  }

  // ---- 地图边界钳制 (Jolt OBJ AABB)；与 WorldSystem::SetBoundsClamp 共用算法 ----
  auto* jolt = server_->GetJoltServer();
  if (jolt && jolt->IsMapLoaded())
  {
    const JPH::Vec3 clamped = server::ClampToMapBounds(jolt->GetBounds(), pos);
    if (clamped != pos)
    {
      LOG_WARN << "move clamped: entity=" << entity->GetId()
               << " req=(" << pos.GetX() << "," << pos.GetY() << "," << pos.GetZ() << ")"
               << " -> clamped=(" << clamped.GetX() << "," << clamped.GetY()
               << "," << clamped.GetZ() << ")"
               << " bounds_min=(" << jolt->GetBounds().min.GetX() << ","
               << jolt->GetBounds().min.GetY() << "," << jolt->GetBounds().min.GetZ()
               << ")"
               << " bounds_max=(" << jolt->GetBounds().max.GetX() << ","
               << jolt->GetBounds().max.GetY() << "," << jolt->GetBounds().max.GetZ()
               << ")";
      pos = clamped;
    }
  }

  // 先写 rot/vel，再 MoveEntity；否则 Jolt OnEntityMove 读到旧 rot
  entity->SetMoveState(rot, move_rot, vel);
  server_->GetWorld()->MoveEntity(entity, pos);
  LOG_INFO << "[MOVE] " << entity->LogTag()
           << " applied_pos=(" << pos.GetX() << "," << pos.GetY()
           << "," << pos.GetZ() << ")"
           << " rot=(" << rot.GetX() << "," << rot.GetY()
           << "," << rot.GetZ() << "," << rot.GetW() << ")";
  // Transform 写入已由 PlayerEntity::OnTransformChanged 自动标脏持久化，无需手动调用。
  SendMoveRes(server_, conn, entity, m.status(), true);
}
