<!-- 拆自 CODE_FLOWS；权威目录 doc/server/ -->

## 28. 历史缺陷与修复备忘

（便于回归时对号入座）

| 级别 | 问题 | 修复要点 |
|------|------|----------|
| P0 | 顶号后旧 disconnect 误 LeaveMap | `cc->conn_ != conn` 则忽略；踢线前 `SetContext{}` |
| P0 | 重连时 IsInMap 仍 true，EnterMap 短路 | 恢复前进 LeaveMap 再 Enter |
| P1 | 重登旧 session 仍有效 | `UnregisterBySessionId` |
| P1 | role_list session 恢复双连接 | 补顶号踢线 + session 二次校验 |
| P1 | 粘包多帧用临时 entity | OnMessage 每帧刷新 Context |
| P1 | kInGame&&!IsInMap 无法再进图 | GameHandler 允许恢复分支 |
| P1 | LeaveMap 无幂等 / 离图后幽灵 dirty | IsInMap 守卫；SetWorld(nullptr)；Mark/Push 守卫 |
| P1 | 脏跨格双重 kUpdate | SwitchMonitor 即时推送后 ClearPropertyTypes |
| P2 | SerializeDirty 漏 move_rot | 与 Appear 对齐 |
| P2 | 回归用例空块 | DirtyUpdateAfterLeave 恢复标脏 |
| — | connection_component 缺 memory | `#include <memory>` |
| P0 | zrpc Write 失败同步 HandleClose 导致 AOI 重入崩 | 整文件对齐 muduo：写失败/读错误/缓冲超限均不关连接；仅 read0 与 ForceClose 关连 |
| P0 | 同连接再登录 → kLoggedIn+IsInMap 软锁 | 同实体路径先 `LeaveMap`；`OnUserLogin` 内 `UnregisterBySessionId(旧)` |
| P1 | enter_game 缺 LeaveMap 守卫（相对 reconnect） | `GameHandler` 进图前 `if (IsInMap) LeaveMap` |
| P1 | role_list session 恢复遗留 IsInMap | 恢复时先 `LeaveMap`；复用 ConnectionComponent 保 `send_seq_` |
| P2 | role_list uid 不一致抢 role_id 索引 | 仅复制 RoleComponent，不 `RegisterByRoleId` |
| P2 | role_login 不踢旧连接幽灵再进图 | LeaveMap + kickoff + `kDisconnected` 后再 `OnRoleLogin` |
| P2 | move 仅校验 pos 有限性 | rot/move_rot/velocity 同步 `isfinite` 拒绝 |
| P1 | role_login 先踢人后校验 uid | 跨账号 `role_id` 可把受害者 LeaveMap/踢线；改为 **uid 归属校验通过后再** LeaveMap/kick |
| P2 | LeaveMap 移动中残留 `dirty_property_types_` | `CancelMove` 标 `kStopMove` 后实体脏位未清；`LeaveMap` 在 `SetWorld(nullptr)` 后 `ClearPropertyTypes` |
| P2 | AOI kUpdate/disappear 推自身 | `NotifyUpdated`/`NotifyDisappeared` 过滤 `watcher_id==subject`（与 appear 对齐） |
| P2 | enter_game 未写 `born_move_rot_` | 首次进图与 `born_rot_` 一并写入 |
| P2 | role_list 无效 session 无回包 | session 失败/非 `kLoggedIn` 回 `cli_role_list_res` err≠0 |
| P1 | role_login 校验前已踢同 role | `CheckRoleLogin` 通过后再 LeaveMap/kick；失败路径不踢人 |
| P1 | Handler 状态/拒绝路径静默丢包 | role/move/enter_game 失败一律回对应 `*_res` |
| P2 | handler protobuf cast 失败静默 return | login/handshake/reconnect/role_list 补 err res |
| P2 | `OnKickoff` 直接 `SetInMap(false)` | 仅改连接态；离图由调用方 `LeaveMap` |
| P2 | `MoveWatcher` 失败被忽略 | 失败时 Remove+Add 自愈；传 `EntityPtr` 避免二次查找 |
| P2 | Init 前绑 AOI 回调无效 | `AoiSystem::Init` 末尾 `RebuildViewNotify` |
| P2 | `kUseEmoji` 脏更新空实现 | `SerializeDirty` 刷新 looks |
| P3 | gate/err_code/map_ins 硬编码 | `server_constants.h` + `KickAndShutdownConnection` / `EnterMapAndPushSelfAppear` |
| P0 | 顶号踢线 `Channel::Remove` 断言崩 | zrpc `ConnectDestroyed`：`kDisconnecting` 路径补 `DisableAll` |

---
