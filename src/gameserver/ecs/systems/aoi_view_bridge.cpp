// AOI 视野事件 → 网络包桥接。

#include "ecs/systems/aoi_view_bridge.h"

#include "client_3d.pb.h"
#include "client_common.pb.h"
#include "ecs/components/connection_component.h"
#include "ecs/components/map_component.h"
#include "ecs/components/role_component.h"
#include "ecs/components/transform_component.h"
#include "ecs/entity/entity.h"
#include "protocol/pack_flags.h"
#include "session/system.h"
#include "ecs/systems/world_system.h"
#include "zrpc/base/logger.h"
#include "zrpc/net/tcp_connection.h"

AoiViewBridge::AoiViewBridge(WorldSystem* world, SendFn send_fn)
    : world_(world), send_fn_(std::move(send_fn)) {}

AoiViewBridge::~AoiViewBridge() = default;

void AoiViewBridge::Install() {
    if (!world_) {
        return;
    }

    // AOI 视野广播（appear/disappear/update）保持回调机制：
    // per-subject/per-watcher 的高频数据流（30Hz FlushDirty），携带 watcher_ids 列表，
    // 经 SetEntityBroadcastCallback 直接分发到桥接层序列化发包，不走 EventBus。
    // 地图生命周期事件（enter/leave/move_map）由 MapViewBridge 独立订阅。
    world_->Aoi().SetEntityBroadcastCallback(
        [this](const AoiBroadcastEvent& ev) { OnBroadcast(ev); });
}

void AoiViewBridge::OnBroadcast(const AoiBroadcastEvent& ev) {
    if (!world_ || ev.watcher_ids.empty()) {
        return;
    }
    switch (ev.kind) {
        case AoiEventKind::kEnter: {
            EntityPtr observee = world_->FindEntity(ev.subject_id);
            if (!observee) {
                return;
            }

            std::vector<std::pair<uint64_t, ::zrpc::TcpConnectionPtr>> targets;
            targets.reserve(ev.watcher_ids.size());
            for (uint64_t wid : ev.watcher_ids) {
                EntityPtr observer = world_->FindEntity(wid);
                if (!observer) {
                    continue;
                }
                auto* cc = observer->GetComponent<ConnectionComponent>();
                if (!cc || !cc->conn_ || !cc->conn_->Connected()) {
                    continue;
                }
                targets.emplace_back(wid, cc->conn_);
            }
            if (targets.empty()) {
                return;
            }
            SerializeMsg sm;
            if (!observee->SerializeAppear(sm, /*observer=*/nullptr)) {
                return;
            }
            if (!sm.msg) {
                return;
            }
            std::string body;
            if (!sm.msg->SerializeToString(&body) || body.empty()) {
                LOG_WARN << "appear SerializeToString failed  subject=" << ev.subject_id;
                return;
            }
            // 预判: 目标列表中是否包含自己
            bool has_self = false;
            for (uint64_t wid : ev.watcher_ids) {
                if (wid == ev.subject_id) { has_self = true; break; }
            }
            auto spawn_pos = observee->GetPosition();
            LOG_INFO << "[AOI] >>> SEND appear  " << observee->LogTag()
                     << "  subject_id=" << ev.subject_id
                     << "  pos=(" << spawn_pos.GetX() << "," << spawn_pos.GetY()
                     << "," << spawn_pos.GetZ() << ")"
                     << "  body=" << body.size() << "B"
                     << "  targets=" << targets.size()
                     << (has_self ? " (has_self)" : "");
            if (!has_self) {
                for (const auto& [wid, conn] : targets) {
                    if (send_fn_) {
                        send_fn_(wid, sm.msg_id, body);
                    }
                }
            } else {
                // 有自己 → 原始消息发给其他, 克隆后 is_self=true 发给自己
                for (const auto& [wid, conn] : targets) {
                    if (wid == ev.subject_id) {
                        ::cli_3d_aoi_appears_ntf self_msg;
                        if (self_msg.ParseFromString(body)) {
                            for (int i = 0; i < self_msg.list_size(); ++i) {
                                self_msg.mutable_list(i)->set_is_self(true);
                            }
                            std::string self_body;
                            if (self_msg.SerializeToString(&self_body) && !self_body.empty()) {
                                if (send_fn_) {
                                    send_fn_(wid, sm.msg_id, self_body);
                                }
                            }
                        }
                    } else {
                        if (send_fn_) {
                            send_fn_(wid, sm.msg_id, body);
                        }
                    }
                }
            }
            break;
        }
        case AoiEventKind::kLeave: {
            EntityPtr observee = world_->FindEntity(ev.subject_id);
            if (!observee) {
                return;
            }
            std::string body;
            if (!BuildDisappearBody(observee, body)) {
                LOG_WARN << "disappear SerializeToString failed  subject=" << ev.subject_id;
                return;
            }
            LOG_INFO << "[AOI] >>> SEND disappear  subject=" << ev.subject_id
                     << "  body=" << body.size() << "B"
                     << "  targets=" << ev.watcher_ids.size();
            for (uint64_t wid : ev.watcher_ids) {
                if (wid == ev.subject_id) {
                    continue;  // 与 appear/update 一致：不推自身 disappear
                }
                EntityPtr observer = world_->FindEntity(wid);
                if (!observer) {
                    continue;
                }
                auto* cc = observer->GetComponent<ConnectionComponent>();
                if (!cc || !cc->conn_ || !cc->conn_->Connected()) {
                    continue;
                }
                if (send_fn_) {
                    send_fn_(wid, proto_id("cli_3d_aoi_disappears_ntf"), body);
                }
            }
            break;
        }
        case AoiEventKind::kUpdate: {
            EntityPtr observee = world_->FindEntity(ev.subject_id);
            if (!observee) {
                return;
            }

            std::vector<std::pair<uint64_t, ::zrpc::TcpConnectionPtr>> targets;
            targets.reserve(ev.watcher_ids.size());
            for (uint64_t wid : ev.watcher_ids) {
                EntityPtr observer = world_->FindEntity(wid);
                if (!observer) {
                    continue;
                }
                auto* cc = observer->GetComponent<ConnectionComponent>();
                if (!cc || !cc->conn_ || !cc->conn_->Connected()) {
                    continue;
                }
                targets.emplace_back(wid, cc->conn_);
            }
            if (targets.empty()) {
                return;
            }
            std::vector<SerializeMsg> msgs;
            if (!observee->SerializeDirty(msgs)) {
                return;
            }
            // 预判: 目标列表中是否包含自己
            bool has_self = false;
            for (uint64_t wid : ev.watcher_ids) {
                if (wid == ev.subject_id) { has_self = true; break; }
            }
            for (SerializeMsg& sm : msgs) {
                if (!sm.msg) {
                    continue;
                }
                // 无自己 → 统一序列化广播
                if (!has_self) {
                    std::string body;
                    if (!sm.msg->SerializeToString(&body) || body.empty()) {
                        LOG_WARN << "update SerializeToString failed  subject=" << ev.subject_id;
                        continue;
                    }
                    for (const auto& [wid, conn] : targets) {
                        if (send_fn_) send_fn_(wid, sm.msg_id, body);
                    }
                    continue;
                }
                // 有自己 → 按 msg_id 分支；仅 aoi_update 需要 is_self 克隆
                std::string base_body;
                if (!sm.msg->SerializeToString(&base_body) || base_body.empty()) {
                    LOG_WARN << "update SerializeToString failed  subject=" << ev.subject_id;
                    continue;
                }
                const bool is_update_ntf =
                    (sm.msg_id == proto_id("cli_3d_aoi_update_ntf"));
                for (const auto& [wid, conn] : targets) {
                    if (wid == ev.subject_id) {
                        if (!is_update_ntf) {
                            // attr 等非 update 消息：直接发给自己，勿按 update 结构解析
                            if (send_fn_) send_fn_(wid, sm.msg_id, base_body);
                            continue;
                        }
                        ::cli_3d_aoi_update_ntf self_msg;
                        if (!self_msg.ParseFromString(base_body)) continue;
                        for (int i = 0; i < self_msg.list_size(); ++i) {
                            self_msg.mutable_list(i)->set_is_self(true);
                        }
                        std::string self_body;
                        if (self_msg.SerializeToString(&self_body) && !self_body.empty()) {
                            if (send_fn_) send_fn_(wid, sm.msg_id, self_body);
                        }
                    } else {
                        if (send_fn_) send_fn_(wid, sm.msg_id, base_body);
                    }
                }
            }
            auto update_pos = observee->GetPosition();
            LOG_INFO << "[AOI] >>> SEND update  " << observee->LogTag()
                     << "  subject_id=" << ev.subject_id
                     << "  pos=(" << update_pos.GetX() << "," << update_pos.GetY()
                     << "," << update_pos.GetZ() << ")"
                     << "  msgs=" << msgs.size()
                     << "  targets=" << targets.size();
            break;
        }
    }
}

void AoiViewBridge::OnEntityAppear(uint64_t viewer_id, uint64_t subject_id) {
    if (!world_) return;
    EntityPtr observer = world_->FindEntity(viewer_id);
    EntityPtr observee = world_->FindEntity(subject_id);
    if (!observer || !observee) return;
    auto* conn_comp = observer->GetComponent<ConnectionComponent>();
    if (!conn_comp || !conn_comp->conn_ || !conn_comp->conn_->Connected()) return;
    SerializeMsg sm;
    if (!observee->SerializeAppear(sm, observer)) return;
    if (!sm.msg) return;
    std::string body;
    if (!sm.msg->SerializeToString(&body) || body.empty()) return;
    if (send_fn_) send_fn_(observer->GetId(), sm.msg_id, body);
}

void AoiViewBridge::OnEntityDisappear(uint64_t viewer_id, uint64_t subject_id) {
    if (!world_) return;
    EntityPtr observer = world_->FindEntity(viewer_id);
    EntityPtr observee = world_->FindEntity(subject_id);
    if (!observer || !observee) return;
    auto* conn_comp = observer->GetComponent<ConnectionComponent>();
    if (!conn_comp || !conn_comp->conn_ || !conn_comp->conn_->Connected()) return;
    std::string body;
    if (!BuildDisappearBody(observee, body)) return;
    if (send_fn_) send_fn_(observer->GetId(), proto_id("cli_3d_aoi_disappears_ntf"), body);
}

void AoiViewBridge::OnEntityUpdate(uint64_t viewer_id, uint64_t subject_id) {
    if (!world_) return;
    EntityPtr observer = world_->FindEntity(viewer_id);
    EntityPtr observee = world_->FindEntity(subject_id);
    if (!observer || !observee) return;
    auto* conn_comp = observer->GetComponent<ConnectionComponent>();
    if (!conn_comp || !conn_comp->conn_ || !conn_comp->conn_->Connected()) return;
    std::vector<SerializeMsg> msgs;
    if (!observee->SerializeDirty(msgs)) return;
    for (SerializeMsg& sm : msgs) {
        if (!sm.msg) continue;
        std::string body;
        if (!sm.msg->SerializeToString(&body) || body.empty()) continue;
        if (send_fn_) send_fn_(observer->GetId(), sm.msg_id, body);
    }
}

bool AoiViewBridge::BuildAppearBody(const EntityPtr& observee,
                                    const EntityPtr& observer,
                                    std::string& out_body) {
    if (!observee) return false;
    SerializeMsg sm;
    if (!observee->SerializeAppear(sm, observer)) return false;
    if (!sm.msg) return false;
    return sm.msg->SerializeToString(&out_body) && !out_body.empty();
}

bool AoiViewBridge::BuildDisappearBody(const EntityPtr& observee,
                                       std::string& out_body) {
    if (!observee) return false;
    auto* role = observee->GetComponent<RoleComponent>();
    ::cli_3d_aoi_disappears_ntf disappear;
    disappear.add_entity_id_list(role ? role->role_id_ : observee->GetId());
    return disappear.SerializeToString(&out_body) && !out_body.empty();
}
