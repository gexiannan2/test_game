#include "ecs/systems/map_view_bridge.h"

#include "client_3d.pb.h"
#include "common/event_bus.h"
#include "ecs/components/connection_component.h"
#include "ecs/components/map_component.h"
#include "ecs/components/role_component.h"
#include "ecs/components/transform_component.h"
#include "ecs/entity/entity.h"
#include "ecs/systems/map_config_system.h"
#include "protocol/pack_flags.h"
#include "zrpc/base/logger.h"
#include "zrpc/net/tcp_connection.h"

MapViewBridge::~MapViewBridge() {
    if (enter_map_listener_id_ != 0) {
        EventBus::Instance().Unsubscribe<EvtEnterMap>(enter_map_listener_id_);
        enter_map_listener_id_ = 0;
    }
    if (leave_map_listener_id_ != 0) {
        EventBus::Instance().Unsubscribe<EvtLeaveMap>(leave_map_listener_id_);
        leave_map_listener_id_ = 0;
    }
}

void MapViewBridge::Install() {
    if (!world_) {
        return;
    }
    enter_map_listener_id_ = EventBus::Instance().Subscribe<EvtEnterMap>(
        [this](const EvtEnterMap& ev) {
            if (ev.entity) {
                OnEntityEnterMap(ev.entity);
            }
        });
    leave_map_listener_id_ = EventBus::Instance().Subscribe<EvtLeaveMap>(
        [this](const EvtLeaveMap& ev) {
            if (ev.entity) {
                OnEntityLeaveMap(ev.entity);
            }
        });
}

void MapViewBridge::OnEntityEnterMap(const EntityPtr& entity) {
    if (!entity || !entity->IsPlayer()) {
        return;
    }
    auto* conn_comp = entity->GetComponent<ConnectionComponent>();
    if (!conn_comp || !conn_comp->conn_ || !conn_comp->conn_->Connected()) {
        return;
    }
    auto* tfm = entity->GetComponent<TransformComponent>();
    auto* map_comp = entity->GetComponent<MapComponent>();
    auto* role = entity->GetComponent<RoleComponent>();
    auto* map_cfg = map_comp ? MapConfigSystem::Instance().Find(map_comp->map_cfg_id_) : nullptr;

    ::cli_3d_enter_map_ntf enter_map;
    enter_map.set_cfg_id(map_comp ? map_comp->map_cfg_id_ : 0);
    enter_map.set_map_id(map_comp ? map_comp->map_ins_id_ : 0);
    enter_map.set_source_id(map_cfg ? map_cfg->res_id_ : "map_default");
    enter_map.set_role_entity_id(role ? role->role_id_ : 0);
    if (tfm) {
        auto* pos = enter_map.mutable_pos();
        pos->set_x(tfm->pos_.GetX());
        pos->set_y(tfm->pos_.GetY());
        pos->set_z(tfm->pos_.GetZ());
        auto* rot = enter_map.mutable_rot();
        rot->set_x(tfm->rot_.GetX());
        rot->set_y(tfm->rot_.GetY());
        rot->set_z(tfm->rot_.GetZ());
        rot->set_w(tfm->rot_.GetW());
    }
    enter_map.set_err_code(0);
    LOG_INFO << "[RES] cli_3d_enter_map_ntf " << entity->LogTag()
             << " cfg_id=" << enter_map.cfg_id()
             << " role_entity_id=" << enter_map.role_entity_id()
             << " pos=(" << (tfm ? tfm->pos_.GetX() : 0) << ","
             << (tfm ? tfm->pos_.GetY() : 0) << ","
             << (tfm ? tfm->pos_.GetZ() : 0) << ")";
    std::string body;
    if (enter_map.SerializeToString(&body) && !body.empty()) {
        if (send_fn_) {
            send_fn_(entity->GetId(), proto_id("cli_3d_enter_map_ntf"), body);
        }
    } else {
        LOG_WARN << "enter_map SerializeToString failed  entity=" << entity->GetId();
    }
}

void MapViewBridge::OnEntityLeaveMap(const EntityPtr& entity) {
    if (!entity || !entity->IsPlayer()) {
        return;
    }
    auto* conn_comp = entity->GetComponent<ConnectionComponent>();
    if (!conn_comp || !conn_comp->conn_ || !conn_comp->conn_->Connected()) {
        return;
    }
    auto* map_comp = entity->GetComponent<MapComponent>();

    ::cli_3d_leave_map_ntf leave_map;
    leave_map.set_map_id(map_comp ? map_comp->map_ins_id_ : 0);
    leave_map.set_op_code(0);
    leave_map.set_err_code(0);
    LOG_INFO << "[RES] cli_3d_leave_map_ntf " << entity->LogTag()
             << " map_id=" << leave_map.map_id();
    std::string body;
    if (leave_map.SerializeToString(&body) && !body.empty()) {
        if (send_fn_) {
            send_fn_(entity->GetId(), proto_id("cli_3d_leave_map_ntf"), body);
        }
    } else {
        LOG_WARN << "leave_map SerializeToString failed  entity=" << entity->GetId();
    }
}
