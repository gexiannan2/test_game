#include "handlers/jump_handler.h"

#include <algorithm>
#include <chrono>

#include "client_3d.pb.h"
#include "client_common.pb.h"
#include "ecs/components/connection_component.h"
#include "ecs/components/role_component.h"
#include "ecs/components/transform_component.h"
#include "game_server.h"
#include "protocol/pack_flags.h"
#include "server_constants.h"
#include "utils/entity_util.h"
#include "utils/map_util.h"
#include "utils/proto_fill.h"
#include "utils/transform_util.h"
#include "zrpc/base/logger.h"

int64_t JumpHandler::NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void JumpHandler::FillJumpRes(::cli_3d_jump_res* res, const EntityPtr& entity,
                              int32_t err_code) {
  auto* role = entity->GetComponent<RoleComponent>();
  auto* tfm = entity->GetComponent<TransformComponent>();
  res->set_entity_id(role ? role->role_id_ : entity->GetId());
  res->set_err_code(err_code);
  if (tfm) {
    proto_fill::FillJumpData(res->mutable_jump(), tfm);
  }
}

void JumpHandler::SendJumpRes(const ::zrpc::TcpConnectionPtr& conn,
                              const EntityPtr& entity, int32_t err_code) {
  auto* conn_comp = entity->GetComponent<ConnectionComponent>();
  if (!conn_comp || !conn || !conn->Connected()) return;
  ::cli_3d_jump_res res;
  FillJumpRes(&res, entity, err_code);
  LOG_INFO << "[RES] cli_3d_jump_res err_code=" << err_code
           << " entity_id=" << res.entity_id()
           << " jump_id=" << (res.has_jump() ? res.jump().jump_id() : 0)
           << " op=" << (res.has_jump() ? res.jump().op() : 0)
           << " type=" << (res.has_jump() ? res.jump().type() : 0)
           << " air_idx=" << (res.has_jump() ? res.jump().air_jump_index() : 0);
  server_->SendMsg(conn, proto_id("cli_3d_jump_res"), res, &conn_comp->send_seq_);
}

void JumpHandler::ClearCooldown(uint64_t entity_id) {
  last_jump_ms_.erase(entity_id);
}

void JumpHandler::ClearCooldown(const EntityPtr& entity) {
  if (!entity) return;
  ClearCooldown(entity->GetId());
  if (auto* tfm = entity->GetComponent<TransformComponent>()) {
    tfm->ResetAirState();
  }
}

bool JumpHandler::ValidateStart(const EntityPtr& entity,
                                const TransformComponent* tfm,
                                ::jump_type type, uint32_t jump_id,
                                int32_t* err_out) {
  if (jump_id == 0) {
    *err_out = server::kJumpErrBadParam;
    return false;
  }

  switch (type) {
    case ::JUMP_TYPE_NORMAL:
    case ::JUMP_TYPE_DRAGON:
      if (tfm->airborne_) {
        *err_out = server::kJumpErrBadAirState;
        return false;
      }
      break;
    case ::JUMP_TYPE_DOUBLE:
      if (!tfm->airborne_) {
        *err_out = server::kJumpErrBadAirState;
        return false;
      }
      if (tfm->air_jump_index_ >= server::kMaxAirJumps) {
        *err_out = server::kJumpErrAirJumpExhausted;
        return false;
      }
      // 二段须复用当前空中会话 jump_id（允许客户端重报同一 id）
      if (tfm->jump_id_ != 0 && jump_id != tfm->jump_id_) {
        *err_out = server::kJumpErrJumpIdMismatch;
        return false;
      }
      break;
    case ::JUMP_TYPE_FALL:
    case ::JUMP_TYPE_SLIDE:
      // 允许地面进入；已在同类型空中可刷新 START（如重新贴坡）
      if (tfm->airborne_ && tfm->jump_type_ != static_cast<int32_t>(type) &&
          tfm->jump_type_ != static_cast<int32_t>(::JUMP_TYPE_FALL) &&
          tfm->jump_type_ != static_cast<int32_t>(::JUMP_TYPE_SLIDE)) {
        // 从主动跳跃切入下落/下滑：允许（失足）
      }
      break;
    default:
      *err_out = server::kJumpErrBadParam;
      return false;
  }

  // 主动起跳类吃冷却；失足下落/下滑不限冷却
  if (type == ::JUMP_TYPE_NORMAL || type == ::JUMP_TYPE_DOUBLE ||
      type == ::JUMP_TYPE_DRAGON) {
    const int64_t now = NowMs();
    auto it = last_jump_ms_.find(entity->GetId());
    if (it != last_jump_ms_.end() &&
        (now - it->second) < server::kJumpCooldownMs) {
      *err_out = server::kJumpErrCooldown;
      return false;
    }
    last_jump_ms_[entity->GetId()] = now;
  }
  return true;
}

bool JumpHandler::ValidateSteerOrLand(const TransformComponent* tfm,
                                      uint32_t jump_id, int32_t* err_out) {
  if (!tfm->airborne_) {
    *err_out = server::kJumpErrBadAirState;
    return false;
  }
  if (jump_id == 0 || (tfm->jump_id_ != 0 && jump_id != tfm->jump_id_)) {
    *err_out = server::kJumpErrJumpIdMismatch;
    return false;
  }
  return true;
}

void JumpHandler::ApplyAirState(TransformComponent* tfm,
                                const ::entity_jump_data& j, ::jump_op op,
                                ::jump_type type) {
  const bool was_airborne = tfm->airborne_;

  tfm->jump_id_ = j.jump_id();
  tfm->jump_op_ = static_cast<int32_t>(op);
  tfm->jump_type_ = static_cast<int32_t>(type);
  tfm->jump_client_time_ = j.client_time();

  if (op == ::JUMP_LAND) {
    tfm->airborne_ = false;
    tfm->air_jump_index_ = 0;
    return;
  }

  tfm->airborne_ = true;
  if (op == ::JUMP_START) {
    if (type == ::JUMP_TYPE_DOUBLE) {
      tfm->air_jump_index_ =
          std::min(tfm->air_jump_index_ + 1u, server::kMaxAirJumps);
    } else if (type == ::JUMP_TYPE_NORMAL || type == ::JUMP_TYPE_DRAGON) {
      tfm->air_jump_index_ = 0;
    } else if (type == ::JUMP_TYPE_FALL || type == ::JUMP_TYPE_SLIDE) {
      if (!was_airborne) {
        tfm->air_jump_index_ = 0;
      }
    }
    if (j.air_jump_index() > 0) {
      tfm->air_jump_index_ = j.air_jump_index();
    }
  }
}

void JumpHandler::Handle(const ::zrpc::TcpConnectionPtr& conn,
                         const EntityPtr& entity, uint32_t msg_id,
                         const std::shared_ptr<::google::protobuf::Message>& req) {
  if (!game_util::RequireConn(entity, msg_id)) {
    return;
  }
  if (msg_id != proto_id("cli_3d_jump_req")) {
    LOG_WARN << "JumpHandler unhandled msg_id=" << msg_id;
    return;
  }
  if (!game_util::RequireInGame(entity)) {
    SendJumpRes(conn, entity, server::kJumpErrBadState);
    return;
  }

  auto req_ptr = std::static_pointer_cast<::cli_3d_jump_req>(req);
  if (!req_ptr || !req_ptr->has_jump()) {
    SendJumpRes(conn, entity, server::kJumpErrBadParam);
    return;
  }

  const auto& j = req_ptr->jump();
  const auto op = j.op();
  const auto type = j.type();

  JPH::Vec3 pos =
      j.has_pos() ? game_util::ExtractVec3(j.pos()) : JPH::Vec3::sZero();
  JPH::Quat rot =
      j.has_rot() ? game_util::ExtractQuat(j.rot()) : JPH::Quat::sIdentity();
  JPH::Vec3 vel = j.has_velocity() ? game_util::ExtractVec3(j.velocity())
                                   : JPH::Vec3::sZero();

  LOG_INFO << "[REQ] cli_3d_jump_req " << entity->LogTag()
           << " jump_id=" << j.jump_id() << " op=" << op << " type=" << type
           << " air_idx=" << j.air_jump_index()
           << " pos=" << game_util::FormatVec3(pos)
           << " vel=" << game_util::FormatVec3(vel);

  if (!game_util::Vec3Finite(pos) || !game_util::Vec3Finite(vel)) {
    SendJumpRes(conn, entity, server::kJumpErrBadPos);
    return;
  }
  if (!game_util::QuatFinite(rot)) {
    SendJumpRes(conn, entity, server::kJumpErrBadParam);
    return;
  }

  auto* tfm = entity->GetComponent<TransformComponent>();
  if (!tfm) {
    SendJumpRes(conn, entity, server::kJumpErrBadState);
    return;
  }

  int32_t err = server::kJumpErrOk;
  if (op == ::JUMP_START) {
    if (!ValidateStart(entity, tfm, type, j.jump_id(), &err)) {
      SendJumpRes(conn, entity, err);
      return;
    }
  } else if (op == ::JUMP_STEER || op == ::JUMP_LAND) {
    if (!ValidateSteerOrLand(tfm, j.jump_id(), &err)) {
      SendJumpRes(conn, entity, err);
      return;
    }
  } else {
    SendJumpRes(conn, entity, server::kJumpErrBadParam);
    return;
  }

  server::ClampToMap(server_->GetJoltServer(), pos);

  // 先写空中状态，再 MoveEntity，保证 Flush 时 FillJumpData 完整
  ApplyAirState(tfm, j, op, type);
  if (op != ::JUMP_LAND) {
    // LAND 已 ResetAirState；仍需同步落点
  }

  entity->SetMoveState(rot, rot, vel, EntityPropertyType::kJump);
  server_->GetWorld()->MoveEntity(entity, pos, EntityPropertyType::kJump);

  // LAND 时 ApplyAirState 已清状态；补写最后一帧 op/type 供 res 回显
  if (op == ::JUMP_LAND) {
    tfm->jump_id_ = j.jump_id();
    tfm->jump_op_ = static_cast<int32_t>(::JUMP_LAND);
    tfm->jump_type_ = static_cast<int32_t>(type);
    tfm->jump_client_time_ = j.client_time();
    tfm->airborne_ = false;
    tfm->air_jump_index_ = 0;
  }

  LOG_INFO << "[JUMP] " << entity->LogTag()
           << " applied_pos=" << game_util::FormatVec3(pos) << " op=" << op
           << " type=" << type << " airborne=" << tfm->airborne_
           << " air_idx=" << tfm->air_jump_index_;

  SendJumpRes(conn, entity, server::kJumpErrOk);

  // LAND 回包后清展示字段，避免残留 JUMP 态
  if (op == ::JUMP_LAND) {
    tfm->ResetAirState();
  }
}
