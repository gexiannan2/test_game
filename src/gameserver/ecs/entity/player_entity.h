#pragma once

#include <cstdint>
#include <string>

#include "ecs/entity/entity.h"
#include "ecs/components/role_component.h"

// 玩家实体：IsPlayer/NeedsAoiWatcher；实现 AOI 全量/脏增量序列化。
// 脏增量按类型分发：
//   kMove/kStopMove → cli_3d_aoi_update_ntf (entity_player_data)
//   kJump          → cli_3d_jump_req (aoi_update_ntf: entity_jump_data)
//   kDodge         → cli_3d_dodge_req (aoi_update_ntf: entity_dodge_data)
//   kAttrUpdate    → cli_3d_aoi_attr_update_ntf
//   kUseEmoji/kHomeObject → 跳过
class PlayerEntity : public Entity
{
    public:
        PlayerEntity(WorldSystem* world, uint64_t entity_id, EntityType type,
                     const EntitySpawn& spawn)
            : Entity(world, entity_id, type, spawn)
        {
        }

        // 会话壳：OnConnection 时创建，world_=nullptr，进图时 SetWorld。
        explicit PlayerEntity(uint64_t entity_id)
            : Entity(nullptr, entity_id, EntityType::kPlayer, EntitySpawn{})
        {
        }

        bool IsPlayer() const override
        {
            return true;
        }

        bool IsPlayerEntity() const override
        {
            return true;
        }

        // Transform 写入钩子：SetPosition/SetMoveState 等写入后自动标脏持久化。
        // 业务点（move_handler 等）无需再手动调 MarkPersistDirty。
        // 加载流程由调用方用 Entity::SuppressPersistDirty 屏蔽。
        void OnTransformChanged(EntityPropertyType type) override;

        bool SerializeAppear(SerializeMsg& out, const EntityPtr& observer) override;
        bool SerializeDirty(std::vector<SerializeMsg>& out_msgs) override;

        // 组装玩家 DB 快照：读自身 Role/Map/Transform 组件填充 entity_player_data。
        std::optional<::entity_player_data> SerializeToDB(uint64_t sequence) const override;

        // 运行时组件 -> PlayerDataComponent.data（落地前同步，业务线程）。
        void SyncToData();

        // PlayerDataComponent.data -> 运行时组件（上线加载后同步，业务线程）。
        void SyncFromData();

        // 标记玩家数据已变更，需异步落地。
        // Transform 变更已由 OnTransformChanged 自动调用，业务点无需手动调；
        // 仅在非 Transform 组件变更（升级/改名等）时手动调用。
        void MarkPersistDirty();
};

using PlayerEntityPtr = std::shared_ptr<PlayerEntity>;
