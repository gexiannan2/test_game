#pragma once

#include "ecs/components/connection_component.h"
#include "ecs/entity/entity.h"
#include "protocol/pack_flags.h"
#include "zrpc/base/logger.h"

// Entity 业务校验工具：前置条件检查（连接/游戏状态）。
// 依赖 Entity/ConnectionComponent，供 handlers 及需要 entity 状态校验的模块复用。

namespace game_util {

// 校验 entity 拥有 ConnectionComponent，失败则 WARN 日志并返回 false。
inline bool RequireConn(const EntityPtr& entity, uint32_t msg_id) {
    if (!entity->GetComponent<ConnectionComponent>()) {
        LOG_WARN << "no ConnectionComponent, drop msg_id=" << msg_id;
        return false;
    }
    return true;
}

// 校验 entity 处于 InGame 且在图中（move/jump 等业务消息前置条件）。
inline bool RequireInGame(const EntityPtr& entity) {
    if (!entity->IsInMap() || entity->GetState() != Entity::State::kInGame) {
        LOG_WARN << "req but not in game, entity=" << entity->GetId();
        return false;
    }
    return true;
}

}  // namespace game_util
