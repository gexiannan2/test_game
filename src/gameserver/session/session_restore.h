#pragma once

#include <functional>

#include "ecs/components/connection_component.h"
#include "ecs/entity/entity.h"

class GameServer;

// 顶号 / 重连 / role_list session 恢复的公共重绑定流程。
struct SessionRestoreOptions {
  // 踢线前先离图（账号重登）
  bool leave_map_before_kick = false;
  // 绑连接后若仍在图则离图（role_list restore）
  bool leave_map_after_bind = false;
  // 清理 kDataLoading 残留的 PlayerDataComponent
  bool clear_data_loading = false;
  // 绑连接后写状态（role_list → kLoggedIn）
  bool apply_state_after = false;
  Entity::State state_after = Entity::State::kLoggedIn;
  // 若恢复前处于游戏中：LeaveMap + SetState(kInGame) + EnterMap（reconnect）
  bool reenter_map_if_was_in_game = false;
};

// mid：踢线+绑连接(+清 loading) 之后、reenter/Cleanup 之前调用。
// was_in_game 在 clear_data_loading 判定后计算（与 reconnect 原逻辑一致）。
using SessionRestoreMidFn =
    std::function<void(const EntityPtr& cached, bool was_in_game)>;

void RebindCachedEntity(GameServer* server,
                        const EntityPtr& cached,
                        const EntityPtr& temp_entity,
                        const ::zrpc::TcpConnectionPtr& conn,
                        uint32_t kick_code,
                        const char* kick_reason,
                        const SessionRestoreOptions& opts,
                        SessionRestoreMidFn mid = nullptr);
