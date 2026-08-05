<!-- 拆自 CODE_FLOWS；权威目录 doc/server/ -->

## 5. System 业务静态门面

定义：`systems/system.h`；实现：`connection_system.cpp` / `account_system.cpp` / `role_system.cpp` / `move_system.cpp` / `game_system.cpp`

| 方法 | 职责 |
|------|------|
| `OnHandshake` | → `kHandshaked`；初始化心跳时间 |
| `OnHeartbeat` | 刷新 `last_heartbeat_sec_` |
| `OnReconnect` | → `kLoggedIn`；刷新心跳 |
| `OnKickoff` | → `kDisconnected`；`SetInMap(false)` |
| `OnUserLogin` | 写 `AccountComponent`；失效旧 session 索引；→ `kLoggedIn`；注册 PES |
| `OnRoleListReq` | 确保 `RoleComponent`；uid 实体不一致时仅复制角色列表（不抢 `role_id` 索引） |
| `OnRoleCreate` | 追加角色；`RegisterByRoleId` |
| `OnRoleDelete` | 删列表项；`Unregister(role_id)` |
| `OnRoleLogin` | 校验/迁移角色；→ `kRoleSelected` |
| `OnRandomNameReq` | 按性别随机名 |
| `OnMove` | **只写** Transform（pos/rot/move_rot/velocity），**不**碰 AOI |
| `BuildEntityLooks` | appear.looks 子消息 |

---

---

## 12. 配置与玩家索引

### 12.1 MapConfigSystem

- `LoadDefaults`：如 cfg **1001** 新手村、**1201** 野外森林
- `GetFirstMap()`：默认进图目标（通常 1001）
- 字段：cfg_id、name、res_id、born_pos/rot/move_rot、born_range 等

### 12.2 PlayerConfig

胶囊 height=1.5、radius=0.3。

### 12.3 PlayerEntitySystem

| API | 用途 |
|-----|------|
| Register/Find ByUid/SessionId/RoleId | 登录/重连/顶号 |
| UnregisterBySessionId | 重登失效旧 session |
| Unregister(role_id) | 删角 |
| CleanupEntity | 清临时实体三条索引 |
| GetAllByUidSnapshot | 心跳扫描 |

---
