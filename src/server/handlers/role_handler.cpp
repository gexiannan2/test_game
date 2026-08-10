#include "handlers/role_handler.h"

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

namespace
{
    void FillRoleList(::cli_role_list_res& res, const RoleComponent* role)
    {
        if (!role)
        {
            return;
        }

        for (const auto& info : role->all_roles_)
        {
            auto* ri = res.add_role_list();
            ri->set_role_id(info.role_id);
            ri->set_name(info.name);
            ri->set_sex(info.sex);
            ri->set_job(info.job);
            ri->set_create_time(info.create_time);
            ri->set_last(info.is_last);
        }
    }

    void SendRoleListErr(GameServer* server, const ::zrpc::TcpConnectionPtr& conn,
                         const EntityPtr& entity, int32_t op_code)
    {
        ::cli_role_list_res res;
        res.set_err_code(server::kRoleErrFailed);
        res.set_op_code(op_code);
        res.set_forbid_edit(0);
        auto* cc = entity->GetComponent<ConnectionComponent>();
        if (cc)
        {
            server->SendMsg(conn, proto_id("cli_role_list_res"), res, &cc->send_seq_);
        }
    }

    void SendRandomNameErr(GameServer* server, const ::zrpc::TcpConnectionPtr& conn,
                           const EntityPtr& entity)
    {
        ::cli_random_name_res res;
        res.set_err_code(server::kRoleErrFailed);
        auto* cc = entity->GetComponent<ConnectionComponent>();
        if (cc)
        {
            server->SendMsg(conn, proto_id("cli_random_name_res"), res, &cc->send_seq_);
        }
    }

    void SendRoleLoginErr(GameServer* server, const ::zrpc::TcpConnectionPtr& conn,
                          const EntityPtr& entity, uint64_t role_id,
                          int32_t op_code)
    {
        ::cli_role_login_res res;
        res.set_err_code(server::kRoleErrFailed);
        res.set_role_id(role_id);
        res.set_op_code(op_code);
        auto* cc = entity->GetComponent<ConnectionComponent>();
        if (cc)
        {
            server->SendMsg(conn, proto_id("cli_role_login_res"), res, &cc->send_seq_);
        }
    }

    bool RequireLoggedIn(GameServer* server, const ::zrpc::TcpConnectionPtr& conn,
                         const EntityPtr& entity, uint32_t msg_id,
                         int32_t role_list_op = server::kRoleListOpQuery)
    {
        if (entity->GetState() == Entity::State::kLoggedIn)
        {
            return true;
        }

        LOG_WARN << "state mismatch: current=" << static_cast<int>(entity->GetState())
                 << " expected=" << static_cast<int>(Entity::State::kLoggedIn)
                 << " msg_id=" << msg_id;

        if (msg_id == proto_id("cli_random_name_req"))
        {
            SendRandomNameErr(server, conn, entity);
        }
        else if (msg_id == proto_id("cli_role_login_req"))
        {
            SendRoleLoginErr(server, conn, entity, 0, 0);
        }
        else
        {
            SendRoleListErr(server, conn, entity, role_list_op);
        }

        return false;
    }

} // namespace

void RoleHandler::Handle(const ::zrpc::TcpConnectionPtr& conn,
                          const EntityPtr& entity,
                          uint32_t msg_id,
                          const std::shared_ptr<::google::protobuf::Message>& req)
{
    EntityPtr cur_entity = entity;

    if (!cur_entity->GetComponent<ConnectionComponent>())
    {
        LOG_WARN << "no ConnectionComponent, drop msg_id=" << msg_id;
        return;
    }

    if (msg_id == proto_id("cli_role_list_req"))
    {
        auto req_ptr = std::static_pointer_cast<::cli_role_list_req>(req);
        if (!req_ptr)
        {
            LOG_WARN << "role_handler static_pointer_cast<cli_role_list_req> failed";
            return;
        }

        LOG_INFO << "[REQ] cli_role_list_req session_id=" << req_ptr->session_id();
        if (cur_entity->GetState() == Entity::State::kConnected &&
            req_ptr->session_id() != 0)
        {
            auto cached =
                PlayerEntitySystem::Instance().FindBySessionId(req_ptr->session_id());
            auto* cached_acc =
                cached ? cached->GetComponent<AccountComponent>() : nullptr;
            if (cached && cached_acc &&
                cached_acc->session_id_ == req_ptr->session_id())
            {
                LOG_INFO << "role_list_req: restoring cached entity="
                         << cached->GetId()
                         << " by session_id=" << req_ptr->session_id();

                server_->KickAndShutdownConnection(
                    cached, server::kKickoffReplaceAccount,
                    "role_list_session_restore", conn);

                if (!cached->HasComponent<ConnectionComponent>())
                {
                    cached->AddComponent<ConnectionComponent>();
                }

                auto* bind_cc = cached->GetComponent<ConnectionComponent>();
                bind_cc->conn_ = conn;
                bind_cc->last_heartbeat_sec_ =
                    static_cast<uint64_t>(std::time(nullptr));

                if (cached->IsInMap())
                {
                    server_->GetWorld()->LeaveMap(cached);
                }

                cached->SetState(Entity::State::kLoggedIn);

                PlayerEntitySystem::Instance().CleanupEntity(cur_entity);
                conn->SetContext(EntityPtr(cached));
                cur_entity = cached;
            }
            else
            {
                LOG_WARN << "role_list_req: session_id=" << req_ptr->session_id()
                         << " not found in cache";
                SendRoleListErr(server_, conn, cur_entity, server::kRoleListOpQuery);
                return;
            }
        }

        if (!RequireLoggedIn(server_, conn, cur_entity, proto_id("cli_role_list_req"),
                             server::kRoleListOpQuery))
        {
            return;
        }

        System::OnRoleListReq(cur_entity);

        ::cli_role_list_res res;
        res.set_err_code(server::kRoleErrOk);
        res.set_op_code(server::kRoleListOpQuery);
        FillRoleList(res, cur_entity->GetComponent<RoleComponent>());
        res.set_forbid_edit(0);
        res.add_line_list(server::kDefaultLineId);
        LOG_INFO << "[RES] cli_role_list_res err_code=" << res.err_code()
                 << " op_code=" << res.op_code()
                 << " role_count=" << res.role_list_size()
                 << " forbid_edit=" << res.forbid_edit();
        server_->SendMsg(conn, proto_id("cli_role_list_res"), res,
                         &cur_entity->GetComponent<ConnectionComponent>()
                              ->send_seq_);
    }
    else if (msg_id == proto_id("cli_random_name_req"))
    {
        if (!RequireLoggedIn(server_, conn, cur_entity, proto_id("cli_random_name_req")))
        {
            return;
        }

        auto req_ptr = std::static_pointer_cast<::cli_random_name_req>(req);
        if (!req_ptr)
        {
            LOG_WARN << "role_handler static_pointer_cast<cli_random_name_req> failed";
            SendRandomNameErr(server_, conn, cur_entity);
            return;
        }

        LOG_INFO << "[REQ] cli_random_name sex=" << req_ptr->sex();
        std::string name = System::OnRandomNameReq(req_ptr->sex());
        ::cli_random_name_res res;
        if (!name.empty())
        {
            res.set_random_name(name);
            res.set_err_code(server::kRoleErrOk);
        }
        else
        {
            res.set_err_code(server::kRoleErrFailed);
        }
        LOG_INFO << "[RES] cli_random_name_res random_name=" << res.random_name()
                 << " err_code=" << res.err_code();
        server_->SendMsg(conn, proto_id("cli_random_name_res"), res,
                         &cur_entity->GetComponent<ConnectionComponent>()
                              ->send_seq_);
    }
    else if (msg_id == proto_id("cli_role_create_req"))
    {
        if (!RequireLoggedIn(server_, conn, cur_entity, proto_id("cli_role_create_req"),
                             server::kRoleListOpCreate))
        {
            return;
        }

        auto req_ptr = std::static_pointer_cast<::cli_role_create_req>(req);
        if (!req_ptr)
        {
            LOG_WARN << "role_handler static_pointer_cast<cli_role_create_req> failed";
            SendRoleListErr(server_, conn, cur_entity, server::kRoleListOpCreate);
            return;
        }

        LOG_INFO << "[REQ] cli_role_create_req name=" << req_ptr->role_info().name()
                 << " sex=" << req_ptr->role_info().sex()
                 << " job=" << req_ptr->role_info().job();
        uint64_t new_role_id = server_->GenRoleId();
        System::OnRoleCreate(cur_entity, new_role_id, req_ptr->role_info().name(),
                             req_ptr->role_info().sex(), req_ptr->role_info().job());

        ::cli_role_list_res res;
        res.set_err_code(server::kRoleErrOk);
        res.set_op_code(server::kRoleListOpCreate);
        FillRoleList(res, cur_entity->GetComponent<RoleComponent>());
        auto* role = cur_entity->GetComponent<RoleComponent>();
        LOG_INFO << "[RES] cli_role_list_res(create) err_code=" << res.err_code()
                 << " op_code=" << res.op_code()
                 << " role_count=" << res.role_list_size()
                 << " new_role_id=" << (role ? role->role_id_ : 0);
        server_->SendMsg(conn, proto_id("cli_role_list_res"), res,
                         &cur_entity->GetComponent<ConnectionComponent>()
                              ->send_seq_);
    }
    else if (msg_id == proto_id("cli_role_delete_req"))
    {
        if (cur_entity->GetState() == Entity::State::kInGame)
        {
            LOG_WARN << "cannot delete role in game";
            SendRoleListErr(server_, conn, cur_entity, server::kRoleListOpDelete);
            return;
        }

        if (!RequireLoggedIn(server_, conn, cur_entity, proto_id("cli_role_delete_req"),
                             server::kRoleListOpDelete))
        {
            return;
        }

        auto req_ptr = std::static_pointer_cast<::cli_role_delete_req>(req);
        if (!req_ptr)
        {
            LOG_WARN << "role_handler static_pointer_cast<cli_role_delete_req> failed";
            SendRoleListErr(server_, conn, cur_entity, server::kRoleListOpDelete);
            return;
        }

        LOG_INFO << "[REQ] cli_role_delete_req role_id=" << req_ptr->role_id();
        System::OnRoleDelete(cur_entity, req_ptr->role_id());
        // 删除 mongo 存档（异步，不阻塞回包）。
        server_->DeleteRoleArchive(req_ptr->role_id());
        ::cli_role_list_res res;
        res.set_err_code(server::kRoleErrOk);
        res.set_op_code(server::kRoleListOpDelete);
        FillRoleList(res, cur_entity->GetComponent<RoleComponent>());
        LOG_INFO << "[RES] cli_role_list_res(delete) err_code=" << res.err_code()
                 << " op_code=" << res.op_code()
                 << " role_count=" << res.role_list_size();
        server_->SendMsg(conn, proto_id("cli_role_list_res"), res,
                         &cur_entity->GetComponent<ConnectionComponent>()
                              ->send_seq_);
    }
    else if (msg_id == proto_id("cli_role_login_req"))
    {
        if (!RequireLoggedIn(server_, conn, cur_entity, proto_id("cli_role_login_req")))
        {
            return;
        }

        auto req_ptr = std::static_pointer_cast<::cli_role_login_req>(req);
        if (!req_ptr)
        {
            LOG_WARN << "role_handler static_pointer_cast<cli_role_login_req> failed";
            SendRoleLoginErr(server_, conn, cur_entity, 0, 0);
            return;
        }

        LOG_INFO << "[REQ] cli_role_login_req role_id=" << req_ptr->role_id()
                 << " op_code=" << req_ptr->op_code();

        const auto check = System::CheckRoleLogin(cur_entity, req_ptr->role_id());
        if (check != System::RoleLoginCheck::kOk)
        {
            LOG_WARN << "role login denied before kick: role_id=" << req_ptr->role_id()
                     << " check=" << static_cast<int>(check);
            SendRoleLoginErr(server_, conn, cur_entity, req_ptr->role_id(),
                             req_ptr->op_code());
            return;
        }

        auto existing =
            PlayerEntitySystem::Instance().FindByRoleId(req_ptr->role_id());
        if (existing && existing != cur_entity)
        {
            if (existing->IsInMap())
            {
                LOG_INFO << "role login kick existing entity=" << existing->GetId()
                         << " (same role_id=" << req_ptr->role_id() << ")";
                server_->GetWorld()->LeaveMap(existing);
            }

            server_->KickAndShutdownConnection(
                existing, server::kKickoffReplaceRole, "role_login", conn);
            existing->SetState(Entity::State::kDisconnected);
        }

        if (!System::OnRoleLogin(cur_entity, req_ptr->role_id()))
        {
            LOG_WARN << "[RES] cli_role_login_res err_code=1 role_id="
                     << req_ptr->role_id();
            SendRoleLoginErr(server_, conn, cur_entity, req_ptr->role_id(),
                             req_ptr->op_code());
            return;
        }

        ::cli_role_login_res res;
        res.set_err_code(server::kRoleErrOk);
        res.set_role_id(req_ptr->role_id());
        res.set_op_code(req_ptr->op_code());
        LOG_INFO << "[RES] cli_role_login_res err_code=" << res.err_code()
                 << " role_id=" << res.role_id()
                 << " op_code=" << res.op_code();
        server_->SendMsg(conn, proto_id("cli_role_login_res"), res,
                         &cur_entity->GetComponent<ConnectionComponent>()
                              ->send_seq_);
    }
    else
    {
        LOG_WARN << "RoleHandler unhandled msg_id=" << msg_id;
    }
}
