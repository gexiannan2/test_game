<!-- 拆自 CODE_FLOWS；权威目录 doc/server/ -->

## 11. ECS：Entity / Component / PlayerEntity

### 11.1 状态机 `Entity::State`

```
kConnected → kHandshaked → kLoggedIn → kRoleSelected → kInGame
任意 → kDisconnected
```

| 推进 | 位置 |
|------|------|
| 建连 | `OnConnection` → kConnected |
| 握手 | `OnHandshake` → kHandshaked |
| 登录 | `OnUserLogin`/重登 → kLoggedIn |
| 选角 | `OnRoleLogin` → kRoleSelected |
| 进游戏 | `GameHandler`/重连恢复 → kInGame |
| 断线/踢/心跳 | → kDisconnected |

`IsInMap` 与状态独立：仅 Enter/LeaveMap（及清理路径）修改。

### 11.2 Component

| 组件 | 字段要点 | 挂载时机 |
|------|----------|----------|
| Connection | conn_, send_seq_, last_heartbeat_sec_ | 建连；断线移除；重连/顶号再挂 |
| Account | uid_, token_, channel_id_, session_id_ | 登录 |
| Role | 当前角色 + all_roles_ | 角色协议 |
| Transform | pos/rot/move_rot/velocity/height/radius | Entity 构造默认有；进图写出生点 |
| Map | map_cfg_id_, map_ins_id_ | 首次 enter_game（重连判断「曾进过游戏」） |
| Move | 路径状态机 | NPC RequestMoveTo |

### 11.3 PlayerEntity

- 连接阶段：`PlayerEntity(id)`，`world_=nullptr`
- `SerializeAppear`：全量 pos/rot/move_rot/velocity + looks；`ins_id` 优先 role_id
- `SerializeDirty`：kMove/kStopMove 须含 **move_rot**（与 Appear 对齐）；kUseEmoji 暂空
- `SetPropertyDirty`：`world_==nullptr` 时不累积脏位

---
