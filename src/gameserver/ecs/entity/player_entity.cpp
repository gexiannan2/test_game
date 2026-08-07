// PlayerEntity 序列化 — appears / dirty / persist 三种路径
//
// appears:  entity_player_data → aoi_appears_ntf
// dirty:    按 PropertyType 分发 → aoi_update_ntf / aoi_attr_update_ntf
// persist:  entity_player_data → mongo

#include "ecs/entity/player_entity.h"

#include <string>

#include "zrpc/base/logger.h"

#include "client_3d.pb.h"
#include "client_common.pb.h"
#include "ecs/components/player_data_component.h"
#include "ecs/components/role_component.h"
#include "ecs/components/transform_component.h"
#include "protocol/pack_flags.h"
#include "utils/proto_fill.h"

namespace
{
    template <typename T>
    void AppendToEntity3d(::entity_3d* e3d, ::entity_data_type type, const T& msg)
    {
        auto* ed = e3d->add_entity_data_list();
        ed->set_type(type);
        ed->set_data(msg.SerializeAsString());
    }
}  // namespace

// ============================================================
// SerializeAppear — 全量玩家数据 → aoi_appears_ntf
// ============================================================

bool PlayerEntity::SerializeAppear(SerializeMsg& out, const EntityPtr& observer)
{
    auto* role = GetComponent<RoleComponent>();
    auto* tfm  = GetComponent<TransformComponent>();

    auto ntf = std::make_shared<::cli_3d_aoi_appears_ntf>();
    auto* e3d = ntf->add_list();
    e3d->set_entity_id(role ? role->role_id_ : GetId());
    e3d->set_type(::ENTITY_PLAYER);
    e3d->set_is_self(observer && observer->GetId() == GetId());

    ::entity_player_data player;
    proto_fill::FillPlayerData(&player, GetId(), tfm, role);
    AppendToEntity3d(e3d, ::ENTITY_DATA_TYPE_PLAYER_DATA, player);

    out.msg_id = proto_id("cli_3d_aoi_appears_ntf");
    out.msg    = std::move(ntf);
    return true;
}

// ============================================================
// SerializeDirty — 增量脏数据 → aoi_update_ntf / aoi_attr_update_ntf
// ============================================================

bool PlayerEntity::SerializeDirty(std::vector<SerializeMsg>& out_msgs)
{
    const auto& dirty = PropertyTypes();
    if (dirty.empty())
    {
        return false;
    }

    auto* role = GetComponent<RoleComponent>();
    auto* tfm  = GetComponent<TransformComponent>();
    const uint64_t eid = role ? role->role_id_ : GetId();

    bool ok = false;

    for (EntityPropertyType typ : dirty)
    {
        SerializeMsg sm;

        switch (typ)
        {
            case EntityPropertyType::kMove:
            case EntityPropertyType::kStopMove:
            {
                auto ntf = std::make_shared<::cli_3d_aoi_update_ntf>();
                auto* e3d = ntf->add_list();
                e3d->set_entity_id(eid);
                e3d->set_type(::ENTITY_PLAYER);

                ::entity_move_data move;
                proto_fill::FillMoveData(&move, tfm);
                AppendToEntity3d(e3d, ::ENTITY_DATA_TYPE_MOVE_DATA, move);

                LOG_INFO << "[AOI] SerializeDirty " << LogTag()
                         << " type=" << static_cast<int>(typ)
                         << " dirty_count=" << dirty.size()
                         << " entity_data_list_size=" << e3d->entity_data_list_size();

                sm.msg_id = proto_id("cli_3d_aoi_update_ntf");
                sm.msg    = std::move(ntf);
                ok = true;
                break;
            }
            case EntityPropertyType::kAttrUpdate:
            {
                auto ntf = std::make_shared<::cli_3d_aoi_attr_update_ntf>();
                ntf->set_entity_id(eid);

                sm.msg_id = proto_id("cli_3d_aoi_attr_update_ntf");
                sm.msg    = std::move(ntf);
                ok = true;
                break;
            }
            case EntityPropertyType::kJump:
            {
                auto ntf = std::make_shared<::cli_3d_aoi_update_ntf>();
                auto* e3d = ntf->add_list();
                e3d->set_entity_id(eid);
                e3d->set_type(::ENTITY_PLAYER);

                ::entity_jump_data jump;
                proto_fill::FillJumpData(&jump, tfm);
                AppendToEntity3d(e3d, ::ENTITY_DATA_TYPE_JUMP_DATA, jump);

                LOG_INFO << "[AOI] SerializeDirty(kJump) " << LogTag()
                         << " jump_id=" << tfm->jump_id_
                         << " op=" << tfm->jump_op_
                         << " entity_data_list_size=" << e3d->entity_data_list_size();

                sm.msg_id = proto_id("cli_3d_aoi_update_ntf");
                sm.msg    = std::move(ntf);
                ok = true;
                break;
            }
            default:
                break;  // kDodge/kUseEmoji/kHomeObject 由各自 handler + bridge 处理
        }

        if (sm.msg)
        {
            out_msgs.push_back(std::move(sm));
        }
    }

    return ok;
}

// ============================================================
// SerializeToDB — 落库快照
// ============================================================

std::optional<::entity_player_data> PlayerEntity::SerializeToDB(uint64_t sequence) const
{
    (void)sequence;

    // 优先返回 PlayerDataComponent.data（新路径；调用前须 SyncToData 刷新）。
    // proto 全字段由 data 承载，运行时未用字段原样保留，落地不丢。
    auto* pdc = GetComponent<PlayerDataComponent>();
    if (pdc != nullptr)
    {
        return pdc->data;
    }

    // 兜底：无 PlayerDataComponent（加载/标脏流程未接通时），走旧逐字段组装。
    auto* role = GetComponent<RoleComponent>();
    auto* tfm  = GetComponent<TransformComponent>();
    if (!role || !tfm)
    {
        return std::nullopt;
    }

    ::entity_player_data data;
    proto_fill::FillPlayerData(&data, GetId(), tfm, role);
    return data;
}

// ============================================================
// OnTransformChanged — Transform 写入自动标脏持久化
// ============================================================

void PlayerEntity::OnTransformChanged(EntityPropertyType type)
{
    SetPropertyDirty(type);
    MarkPersistDirty();
}

// ============================================================
// SyncToData / SyncFromData / MarkPersistDirty — PlayerDataComponent 同步
// ============================================================

void PlayerEntity::SyncToData()
{
    if (!HasComponent<PlayerDataComponent>())
    {
        AddComponent<PlayerDataComponent>();
    }
    auto* pdc  = GetComponent<PlayerDataComponent>();
    auto* tfm  = GetComponent<TransformComponent>();
    auto* role = GetComponent<RoleComponent>();

    // 复用 FillPlayerData：运行时态(位置/属性)写回 data.base/base_data。
    // 纯存储字段(face_id/appear_list 等)在 data 中原样保留，不覆盖。
    proto_fill::FillPlayerData(&pdc->data, GetId(), tfm, role);
}

void PlayerEntity::SyncFromData()
{
    auto* pdc = GetComponent<PlayerDataComponent>();
    if (pdc == nullptr)
    {
        return;
    }
    auto* tfm  = GetComponent<TransformComponent>();
    auto* role = GetComponent<RoleComponent>();

    // data.base -> TransformComponent
    const auto& b = pdc->data.base();
    if (tfm != nullptr)
    {
        tfm->pos_      = JPH::Vec3(b.pos().x(), b.pos().y(), b.pos().z());
        tfm->rot_      = JPH::Quat(b.rot().x(), b.rot().y(), b.rot().z(), b.rot().w());
        tfm->move_rot_ = JPH::Quat(b.move_rot().x(), b.move_rot().y(), b.move_rot().z(), b.move_rot().w());
        tfm->velocity_ = JPH::Vec3(b.velocity().x(), b.velocity().y(), b.velocity().z());
    }

    // data.base_data -> RoleComponent
    const auto& bd = pdc->data.base_data();
    if (role != nullptr)
    {
        role->level_ = bd.lv();
        role->job_   = bd.job();
        role->sex_   = bd.sex();
        if (!bd.name().empty())
        {
            role->name_ = bd.name();
        }
    }

    // battle 字段：当前无独立战斗组件，留在 data.battle 中按需读取
    // （AOI 序列化时直接用，待战斗系统扩展时再消费）
}

void PlayerEntity::MarkPersistDirty()
{
    if (!HasComponent<PlayerDataComponent>())
    {
        AddComponent<PlayerDataComponent>();
    }
    GetComponent<PlayerDataComponent>()->SetDirty();

    // 诊断日志：确认脏标记被设置 + 当前坐标
    auto* tfm = GetComponent<TransformComponent>();
    LOG_INFO << "MarkPersistDirty: entity=" << GetId()
             << " pos=(" << (tfm ? tfm->pos_.GetX() : 0.0f) << ","
             << (tfm ? tfm->pos_.GetY() : 0.0f) << ","
             << (tfm ? tfm->pos_.GetZ() : 0.0f) << ")";
}
