#include "handlers/account_handler.h"

#include <ctime>

#include "client_login.pb.h"
#include "ecs/components/connection_component.h"
#include "ecs/components/account_component.h"
#include "ecs/components/role_component.h"
#include "ecs/components/transform_component.h"
#include "ecs/entity/entity.h"
#include "session/session_restore.h"
#include "session/system.h"
#include "game_server.h"
#include "protocol/pack_flags.h"
#include "server_constants.h"
#include "zrpc/base/logger.h"
#include "zrpc/net/tcp_connection.h"

namespace {

void SendLoginRes(GameServer* server, const ::zrpc::TcpConnectionPtr& conn,
                  uint8_t* seq, int32_t err, uint64_t session_id) {
  ::cli_user_login_res res;
  res.set_err_code(err);
  res.set_session_id(session_id);
  res.set_gate_addr(server->GateAddr());
  server->SendMsg(conn, proto_id("cli_user_login_res"), res, seq);
}

}  // namespace

void AccountHandler::Handle(const ::zrpc::TcpConnectionPtr& conn,
                             const EntityPtr& entity,
                             uint32_t msg_id,
                             const std::shared_ptr<::google::protobuf::Message>& req) {
  auto* conn_comp = entity->GetComponent<ConnectionComponent>();
  if (!conn_comp) {
    LOG_WARN << "no ConnectionComponent, drop msg_id=" << msg_id;
    return;
  }
  if (msg_id != proto_id("cli_user_login_req")) {
    LOG_WARN << "AccountHandler unhandled msg_id=" << msg_id;
    return;
  }

  auto req_ptr = std::static_pointer_cast<::cli_user_login_req>(req);
  if (!req_ptr) {
    LOG_WARN << "account_handler static_pointer_cast<cli_user_login_req> failed";
    SendLoginRes(server_, conn, &conn_comp->send_seq_,
                 server::kLoginErrBadRequest, 0);
    return;
  }
  LOG_INFO << "[REQ] cli_user_login_req uid=" << req_ptr->uid()
           << " channel_id=" << req_ptr->channel_id();

  if (req_ptr->uid().empty()) {
    SendLoginRes(server_, conn, &conn_comp->send_seq_,
                 server::kLoginErrEmptyUid, 0);
    LOG_WARN << "login rejected: empty uid";
    return;
  }

  // 重登：复用缓存实体，不复制组件
  auto existing = PlayerEntitySystem::Instance().FindByUid(req_ptr->uid());
  if (existing && existing != entity) {
    LOG_INFO << "user re-login (restore): uid=" << req_ptr->uid()
             << " old_entity=" << existing->GetId()
             << " new_entity=" << entity->GetId();

    SessionRestoreOptions opts;
    opts.leave_map_before_kick = true;
    uint64_t session_id = 0;
    RebindCachedEntity(
        server_, existing, entity, conn, server::kKickoffReplaceAccount,
        "re_login", opts,
        [this, &req_ptr, &session_id](const EntityPtr& cached,
                                      bool /*was_in_game*/) {
          session_id = SessionService::OnUserLogin(
              cached, req_ptr->uid(), req_ptr->token(), req_ptr->channel_id(),
              server_->GenSessionId());
          if (auto archive = server_->GetPlayerArchive()) {
            archive->PostAccountInfoAfterLogin(cached);
          }
        });

    auto* tfm = existing->GetComponent<TransformComponent>();
    auto* cc = existing->GetComponent<ConnectionComponent>();
    LOG_INFO << "user re-login restored: uid=" << req_ptr->uid()
             << " entity=" << existing->GetId()
             << " session_id=" << session_id
             << " has_pos=" << (tfm ? "yes" : "no");

    SendLoginRes(server_, conn, &cc->send_seq_, server::kLoginErrOk,
                 session_id);
    return;
  }

  if (entity->IsInMap()) {
    server_->GetWorld()->LeaveMap(entity);
  }

  uint64_t session_id = SessionService::OnUserLogin(
      entity, req_ptr->uid(), req_ptr->token(), req_ptr->channel_id(),
      server_->GenSessionId());
  if (auto archive = server_->GetPlayerArchive()) {
    archive->PostAccountInfoAfterLogin(entity);
  }

  LOG_INFO << "[RES] cli_user_login_res err_code=0 session_id=" << session_id
           << " gate_addr=\"" << server_->GateAddr() << "\"";
  SendLoginRes(server_, conn, &conn_comp->send_seq_, server::kLoginErrOk,
               session_id);
}
