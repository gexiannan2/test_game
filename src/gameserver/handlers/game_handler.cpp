#include "handlers/game_handler.h"

#include "client_3d.pb.h"
#include "client_login.pb.h"
#include "ecs/components/account_component.h"
#include "ecs/components/connection_component.h"
#include "ecs/components/map_component.h"
#include "ecs/components/player_data_component.h"
#include "ecs/components/role_component.h"
#include "ecs/components/transform_component.h"
#include "ecs/entity/entity.h"
#include "ecs/entity/player_entity.h"
#include "ecs/systems/map_config_system.h"
#include "game_server.h"
#include "protocol/pack_flags.h"
#include "server_constants.h"
#include "zrpc/base/logger.h"
#include "zrpc/net/tcp_connection.h"

namespace {

// 发送 enter_game 错误响应
void SendEnterGameErr(GameServer* server, const ::zrpc::TcpConnectionPtr& conn,
                      const EntityPtr& entity, int32_t err) {
    ::cli_enter_game_res res;
    res.set_err_code(err);
    auto* cc = entity->GetComponent<ConnectionComponent>();
    if (cc) {
        server->SendMsg(conn, proto_id("cli_enter_game_res"), res, &cc->send_seq_);
    }
}

// 发送 enter_game 成功响应 + 全局配置空包
void SendEnterGameOk(GameServer* server, const ::zrpc::TcpConnectionPtr& conn,
                     ConnectionComponent* cc) {
    ::cli_enter_game_res res;
    res.set_err_code(server::kEnterGameErrOk);
    LOG_INFO << "[RES] cli_enter_game_res err_code=" << res.err_code();
    server->SendMsg(conn, proto_id("cli_enter_game_res"), res, &cc->send_seq_);

    ::cli_global_config_ntf cfg;
    LOG_INFO << "[RES] cli_global_config_ntf (empty body)";
    server->SendMsg(conn, proto_id("cli_global_config_ntf"), cfg, &cc->send_seq_);
}

}  // namespace

void GameHandler::Handle(const ::zrpc::TcpConnectionPtr& conn,
                         const EntityPtr& entity,
                         uint32_t msg_id,
                         const std::shared_ptr<::google::protobuf::Message>& req) {
    (void)req;

    auto* conn_comp = entity->GetComponent<ConnectionComponent>();
    if (conn_comp == nullptr) {
        LOG_WARN << "no ConnectionComponent, drop msg_id=" << msg_id;
        return;
    }
    if (msg_id != proto_id("cli_enter_game_req")) {
        LOG_WARN << "GameHandler unhandled msg_id=" << msg_id;
        return;
    }

    // ---- 前置状态校验 ----
    if (entity->GetState() == Entity::State::kInGame && entity->IsInMap()) {
        LOG_WARN << "enter game: already in game";
        SendEnterGameErr(server_, conn, entity, server::kEnterGameErrAlreadyInMap);
        return;
    }
    // 加载中重入：拒绝，防止二次发起异步加载导致回调身份错乱。
    if (entity->GetState() == Entity::State::kDataLoading) {
        LOG_WARN << "enter game: data loading, reject reentry entity=" << entity->GetId();
        SendEnterGameErr(server_, conn, entity, server::kEnterGameErrBadState);
        return;
    }
    const bool can_enter =
        entity->GetState() == Entity::State::kRoleSelected ||
        (entity->GetState() == Entity::State::kInGame && !entity->IsInMap());
    if (!can_enter) {
        LOG_WARN << "enter game without role selected, state="
                 << static_cast<int>(entity->GetState());
        SendEnterGameErr(server_, conn, entity, server::kEnterGameErrBadState);
        return;
    }

    auto* role = entity->GetComponent<RoleComponent>();
    if (role == nullptr || role->role_id_ == 0) {
        LOG_WARN << "enter game: no role selected";
        SendEnterGameErr(server_, conn, entity, server::kEnterGameErrNoRole);
        return;
    }

    LOG_INFO << "[REQ] cli_enter_game_req role_id=" << role->role_id_;

    auto* default_map = MapConfigSystem::Instance().GetFirstMap();
    if (default_map == nullptr) {
        LOG_WARN << "enter game: no map config available, refuse spawn";
        SendEnterGameErr(server_, conn, entity, server::kEnterGameErrNoMap);
        return;
    }

    if (conn_comp->conn_ == nullptr || !conn_comp->conn_->Connected()) {
        LOG_WARN << "enter game: connection lost";
        SendEnterGameErr(server_, conn, entity, server::kEnterGameErrConnLost);
        return;
    }

    const uint64_t role_id = role->role_id_;
    const bool first_enter = !entity->HasComponent<MapComponent>();

    if (!first_enter) {
        HandleReEnter(conn, entity, conn_comp);
        return;
    }
    HandleFirstEnter(conn, entity, conn_comp, role_id, default_map);
}

// 非首次进图（断线重连后重进）：内存实体仍在，用当前位置直接进图，
// 不重新加载存档，避免用滞后存档覆盖运行时位置（防回档）。
void GameHandler::HandleReEnter(const ::zrpc::TcpConnectionPtr& conn,
                                const EntityPtr& entity,
                                ConnectionComponent* conn_comp) {
    // 与 reconnect 一致：残留 IsInMap 时 EnterMap 会短路，须先离图。
    if (entity->IsInMap()) {
        server_->GetWorld()->LeaveMap(entity);
    }
    entity->SetState(Entity::State::kInGame);
    SendEnterGameOk(server_, conn, conn_comp);
    server_->GetWorld()->EnterMap(entity);
    LOG_INFO << "entered game (re-enter) entity=" << entity->GetId()
             << " AOI visible_count="
             << server_->GetWorld()->GetVisibleEntities(entity->GetId()).size();
}

// 首次进图：异步加载 DB 存档后再进图。
void GameHandler::HandleFirstEnter(const ::zrpc::TcpConnectionPtr& conn,
                                   const EntityPtr& entity,
                                   ConnectionComponent* conn_comp,
                                   uint64_t role_id,
                                   const MapConfig* default_map) {
    // 加载期间置 kDataLoading，各 handler 按 state 校验拒绝业务消息
    // （move 要求 kInGame、role 要求 kLoggedIn）。
    // 顶号/重连/会话恢复会改写 entity 状态，回调据此校验丢弃过期结果。
    entity->SetState(Entity::State::kDataLoading);

    // 用 weak_ptr 捕获 entity：防顶号/断线后 entity 析构，回调访问悬空对象（UAF）。
    const std::weak_ptr<Entity> weak_entity(entity);

    auto archive = server_->GetPlayerArchive();
    auto main_cb = [weak_entity, server = server_, role_id, default_map]
        (bool success, ::entity_player_data data)
    {
        auto ent = weak_entity.lock();
        if (ent == nullptr) {
            // UAF 防护①：entity 已析构（顶号清理/断线后无引用），丢弃。
            LOG_INFO << "enter game load: entity gone, drop role_id=" << role_id;
            return;
        }
        // UAF 防护②：顶号/重连/会话恢复会改写 state；非 kDataLoading 说明
        // 已被顶替或断线恢复，回调结果过期，丢弃（不访问组件、不进图）。
        if (ent->GetState() != Entity::State::kDataLoading) {
            LOG_WARN << "enter game load: state changed, drop role_id=" << role_id
                     << " state=" << static_cast<int>(ent->GetState());
            return;
        }

        auto* cc = ent->GetComponent<ConnectionComponent>();
        if (cc == nullptr || cc->conn_ == nullptr || !cc->conn_->Connected()) {
            // 连接已断：回退状态，由 OnConnection(false) 走断线清理。
            ent->SetState(Entity::State::kRoleSelected);
            LOG_WARN << "enter game load: connection lost, abort role_id=" << role_id;
            return;
        }

        auto player = std::dynamic_pointer_cast<PlayerEntity>(ent);
        if (player == nullptr) {
            ent->SetState(Entity::State::kRoleSelected);
            LOG_ERROR << "enter game load: not PlayerEntity role_id=" << role_id;
            return;
        }

        // 内存载体（持久化真源），加载/落地共用。
        if (!ent->HasComponent<PlayerDataComponent>()) {
            ent->AddComponent<PlayerDataComponent>();
        }
        auto* pdc = ent->GetComponent<PlayerDataComponent>();

        // TransformComponent 由 Entity 构造时挂载，防御性补挂。
        if (!ent->HasComponent<TransformComponent>()) {
            ent->AddComponent<TransformComponent>();
        }
        auto* tfm = ent->GetComponent<TransformComponent>();

        if (success && data.base().id() == static_cast<std::int64_t>(role_id)) {
            // 有存档：整体写入持久化真源，SyncFromData 回填运行时组件（位置/属性）。
            // SyncFromData 直接写裸字段（不经 setter），不触发 OnTransformChanged 标脏。
            pdc->data = std::move(data);
            player->SyncFromData();
            LOG_INFO << "enter game load: archive hit role_id=" << role_id
                     << " pos=(" << tfm->pos_.GetX() << "," << tfm->pos_.GetY()
                     << "," << tfm->pos_.GetZ() << ")";
        } else {
            // 无存档（新角色）或主键不符：用默认出生点 + 当前 RoleComponent 组装 data。
            // 直接写裸字段（不经 SetPosition 等 setter），不触发 OnTransformChanged 标脏。
            tfm->pos_      = default_map->born_pos_;
            tfm->rot_      = default_map->born_rot_;
            tfm->move_rot_ = default_map->born_move_rot_;
            // 运行时态（RoleComponent + 默认位置）写回 data，建立内存镜像。
            // 不调 SyncFromData，避免空 base_data 覆盖选角时设的 name/level。
            player->SyncToData();
            pdc->data.mutable_base()->set_id(static_cast<std::int64_t>(role_id));
            if (!success) {
                LOG_INFO << "enter game load: no archive (new role) role_id=" << role_id;
            } else {
                LOG_WARN << "enter game load: archive id mismatch, use default role_id="
                         << role_id << " archive_id=" << data.base().id();
            }
        }

        LOG_INFO << "[SPAWN] " << ent->LogTag()
                 << " pos=(" << tfm->pos_.GetX() << "," << tfm->pos_.GetY()
                 << "," << tfm->pos_.GetZ() << ")"
                 << " rot=(" << tfm->rot_.GetX() << "," << tfm->rot_.GetY()
                 << "," << tfm->rot_.GetZ() << "," << tfm->rot_.GetW() << ")";

        // 挂 MapComponent 前再次校验 state：防重连/会话恢复在回调执行中途
        // 改写 state 后仍继续 EnterMap 造成双进图（回调段间竞态）。
        if (ent->GetState() != Entity::State::kDataLoading) {
            LOG_WARN << "enter game load: state changed before EnterMap, drop role_id="
                     << role_id << " state=" << static_cast<int>(ent->GetState());
            return;
        }

        // 挂 MapComponent（标志进图）。
        if (!ent->HasComponent<MapComponent>()) {
            ent->AddComponent<MapComponent>();
        }
        auto* map = ent->GetComponent<MapComponent>();
        map->map_cfg_id_ = default_map->cfg_id_;
        map->map_ins_id_ = server::kDefaultMapInstanceId;

        ent->SetState(Entity::State::kInGame);
        SendEnterGameOk(server, cc->conn_, cc);
        server->GetWorld()->EnterMap(ent);
        LOG_INFO << "entered game: role_id=" << role_id
                 << " AOI visible_count="
                 << server->GetWorld()->GetVisibleEntities(ent->GetId()).size();
    };

    if (archive) {
        // mongo 启用：异步加载存档，回调在业务线程执行。
        archive->QueryRole(role_id, std::move(main_cb));
    } else {
        // mongo 未启用：等价原 GameServer::QueryRole(nullptr) 立即同步回 cb(false, {})。
        // 走主回调里"无存档→默认出生点 + SyncToData"分支，复用同一份逻辑避免漂移。
        server_->RunInLoop(
            [main_cb = std::move(main_cb)]() { main_cb(false, {}); });
    }
}
