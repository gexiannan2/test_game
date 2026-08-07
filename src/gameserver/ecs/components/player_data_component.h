#pragma once

#include "ecs/component_base/component_base.h"
#include "client_common.pb.h"  // entity_player_data (base + base_data + battle)

// 玩家完整数据内存载体（DB 持久化真源）。
// 严格围绕 entity_player_data 组装（base + base_data + battle），不引入独立业务模块分层。
// 运行时态（Transform/Role/Map 组件）与此处 data 经 PlayerEntity::SyncFromData/SyncToData 同步。
// proto 全字段自动随 JSON 落库/加载，运行时未用字段原样保留不丢。
// 须在业务线程访问（与其它 ECS 组件一致，非线程安全）。
class PlayerDataComponent : public IComponent
{
    public:
        ComponentType Type() const override { return ComponentType::kPlayerData; }

        // 完整玩家数据：base(空间) + base_data(属性) + battle(战斗)
        ::entity_player_data data;

        // 统一标脏（移动/属性变更等业务点 SetDirty，定时器扫描落地后 ClearDirty）
        bool dirty_ = false;
        void SetDirty() { dirty_ = true; }
        void ClearDirty() { dirty_ = false; }
        bool IsDirty() const { return dirty_; }
};

DECLARE_COMPONENT(PlayerDataComponent, ComponentType::kPlayerData)
