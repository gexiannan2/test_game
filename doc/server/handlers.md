<!-- 拆自 CODE_FLOWS；权威目录 doc/server/ -->

## 4. Handler 协议层（逐包）

注册于 `GameServer::Start`。

### 4.1 ConnectionHandler

| msg_id | 行为 |
|--------|------|
| `kCliHandshakeReq` | `System::OnHandshake` → `kCliHandshakeRes` |
| `kCliHeartBeat` | `System::OnHeartbeat`（无回包） |
| `kCliReconnect` | 校验 session（`FindBySessionId` + `Account.session_id_` 一致）→ 顶旧连接（`SetContext{}`+`Shutdown`）→ 挂新 conn → `OnReconnect` → 若有 `MapComponent`：必要时先 `LeaveMap` 再 `EnterMap` + 自身 appear → `SetContext(cached)` |
| `kCliKickoffPlayer` | `LeaveMap`（若在图）→ `OnKickoff` → `Shutdown` |

### 4.2 AccountHandler

| msg_id | 行为 |
|--------|------|
| `kCliUserLoginReq` | uid 空 → err=1；`FindByUid` 命中且≠当前 → **重登**：踢旧连接、复用实体、**新 session 并 UnregisterBySessionId(旧)**、更新索引、`CleanupEntity(临时)`、`SetContext(cached)`；否则（含同实体再登录）若 `IsInMap` 先 `LeaveMap` → `System::OnUserLogin`（失效旧 session）→ `kCliUserLoginRes` |

### 4.3 RoleHandler

| msg_id | 行为 |
|--------|------|
| `kCliRoleListReq` | 若 `kConnected`+session：按 session 恢复（含顶号踢旧连接；若 `IsInMap` 先 `LeaveMap`）；session 无效或非 `kLoggedIn` → `kCliRoleListRes` err≠0；成功 → `OnRoleListReq` → `kCliRoleListRes`(op=1) |
| `kCliRandomName` | `OnRandomNameReq` → `kCliRandomNameRes` |
| `kCliRoleCreateReq` | `GenRoleId` + `OnRoleCreate` → `kCliRoleListRes`(op=2) |
| `kCliRoleDeleteReq` | `kInGame` 禁止；否则 `OnRoleDelete` → `kCliRoleListRes`(op=3) |
| `kCliRoleLoginReq` | **先 `CheckRoleLogin`**（uid 归属 / 角色存在），拒绝则 `err=1` 且不踢人；通过后 `LeaveMap(existing)` + 踢线(`code_id=3`) + `kDisconnected` → `OnRoleLogin` → `kRoleSelected` → `kCliRoleLoginRes`；状态不符也回错误包 |
| `kCliRandomName` / create / delete | 状态不符回对应 `*_res` err≠0（不再静默丢包） |

### 4.4 GameHandler

| msg_id | 行为 |
|--------|------|
| `kCliEnterGameReq` | 已在图 → err=3；允许 `kRoleSelected` **或** `kInGame&&!IsInMap`（恢复）；无角色 → err=5；无地图配置 → err=2；断线 → err=4；首次写出生点+`MapComponent`(ins_id=`kDefaultMapInstanceId`) → `kInGame` → 回 enter_game_res + global_config → **`EnterMapAndPushSelfAppear`** |

### 4.5 MoveHandler

| msg_id | 行为 |
|--------|------|
| `kCli3dMoveReq` | 须 `IsInMap && kInGame` → 校验 pos/rot/move_rot/velocity 有限 → **先 `MoveEntity(pos)` 后 `OnMove(rot/vel)`** → 回自己 `kCli3dMoveRes`（`success`/`status`；拒绝也回包 success=0） |

---
