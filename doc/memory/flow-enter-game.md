# 记忆：进图 / 出生点 / 双向视野

**关键词**：enter_game、EnterMap、AddWatcher、NotifySelfAppear、appear、born_pos、IsInMap 残留  
**最后扫描**：2026-08-05 | 测试 **127/127** 通过

---

## 调用链

```
cli_enter_game_req
  → GameHandler
      → 校验 kRoleSelected 或 (kInGame && !IsInMap)
      → 首次：MapConfig 出生点 + MapComponent + Transform
      → if IsInMap: LeaveMap          // 与 reconnect 一致，防 EnterMap 短路
      → kInGame
      → cli_enter_game_res + cli_global_config_ntf
      → WorldSystem::EnterMap(entity)
          → map_->OnEntityIntoMap（足迹）
          → aoi_.OnEntityIntoMap（subject 注册）
          → aoi_.AddWatcher(center=GetGridCenter)（你看到别人）
          → aoi_.NotifySelfAppear（仅一次，is_self=true）
      → map_->NotifyEnterMap → AoiViewBridge → cli_3d_enter_map_ntf
      → AOI 广播 → cli_3d_aoi_appears_ntf
```

---

## 视野方向

| 方向 | 机制 | 备注 |
|------|------|------|
| 别人看到你 | `OnEntityIntoMap` → `MonitorEntity` → `NotifyAppearAllReceivers` | subject 进家格时触发 |
| 你看到别人 | `AddWatcher` → `AddReceiver` → `NotifyAppearToReceiver` | 跳过 `sid == watcher_id` |
| 你看到自己 | `NotifySelfAppear` → `BroadcastAppear(subject, [self])` | **仅进图一次**；桥接层设 `is_self=true` |
| 别人进图你不重复收自己 | `NotifyAppearToReceiver` 过滤自身 | B 进图时 A 只收 B，不再收 A |

**不推自身的 update/disappear**：`CollectReceiversExcept` + `AoiViewBridge` 双过滤。

---

## 典型场景：A 先进、B 后进（同出生点邻域）

| 步骤 | A 客户端 | B 客户端 |
|------|----------|----------|
| A 进图 | 1× self appear（`is_self=true`） | — |
| B 进图（在 A 视野内） | 收到 B appear；**不再**收到 A appear | self appear + A appear |
| B 移出 A 视野 | B disappear | A disappear |
| B 移回 | B appear | A appear |

联调用例：`src/client/aoi_enter_leave_test.cc`（Phase 1–3）。

---

## 关键文件

| 文件 | 职责 |
|------|------|
| `handlers/game_handler.cpp` | 进图协议、LeaveMap 守卫 |
| `ecs/systems/world_system.cpp` | EnterMap 总线 |
| `ecs/systems/aoi_system.cpp` + `aoi_sector.cpp` | 视野格、Monitor、NotifySelfAppear |
| `ecs/systems/aoi_view_bridge.cpp` | AOI 事件 → protobuf（is_self 克隆） |
| `ecs/systems/map_config_system.*` | born_pos/rot/move_rot |

---

## 硬约束

1. **kInGame ≠ IsInMap**（enter_game 中途失败可能留组件）  
2. **进图前 IsInMap 必须先 LeaveMap**（reconnect / 顶号 / enter_game）  
3. **首次进图判 MapComponent**，不是 TransformComponent  
4. **born_move_rot_** 与 born_rot_ 一并写入  
5. **QueryRole/SyncFromData 未接入**：重启仍用出生点（已知项）  
6. **自身 appear 只走 `NotifySelfAppear` 一次**，勿在 Handler 再发一份

---

## 相关测试

```bash
cd /mnt/d/Dev/tmp1/game
make -j$(nproc) svc_game_3d_test
GAME_TEST_FILTER=AoiAppearDisappear ./bin/svc_game_3d_test
GAME_TEST_FILTER=LoginAoiChurn ./bin/svc_game_3d_test
GAME_TEST_FILTER=AoiBroadcast ./bin/svc_game_3d_test
```

- 服务端：`AoiAppearDisappearTest`、`AoiMapTest`、`LoginAoiChurnTest`、`AoiBroadcastTest`  
- 客户端 E2E：`client/aoi_enter_leave_test.cc`
