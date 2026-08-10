<!-- 拆自 CODE_FLOWS；权威目录 doc/server/ -->

## 21. 协议号全表与 err_code

定义文件：`src/protocol/pack_flags.h`（数值为稳定 hash，勿手改）。

### 21.1 连接 / 会话

| 宏 | 名称 | 方向 | 处理方 |
|----|------|------|--------|
| `kCliHandshakeReq` = 538625942 | cli_handshake_req | C→S | ConnectionHandler |
| `kCliHandshakeRes` = 3457459898 | cli_handshake_res | S→C | ConnectionHandler |
| `kCliHeartBeat` = 2261573870 | cli_heart_beat | C→S | ConnectionHandler（无回包） |
| `kCliReconnect` = 1567541006 | cli_reconnect | C→S | ConnectionHandler |
| `kCliReconnectRes` = 2923585316 | cli_reconnect_res | S→C | ConnectionHandler |
| `kCliKickoffPlayer` = 829277783 | cli_kickoff_player | 双向 | 收：ConnectionHandler；发：顶号/踢人 |

### 21.2 账号 / 角色

| 宏 | 名称 | 方向 | 处理方 |
|----|------|------|--------|
| `kCliUserLoginReq/Res` | cli_user_login_* | C↔S | AccountHandler |
| `kCliRoleListReq/Res` | cli_role_list_* | C↔S | RoleHandler（create/delete 也回 list_res） |
| `kCliRoleCreateReq` | cli_role_create_req | C→S | RoleHandler |
| `kCliRoleDeleteReq` | cli_role_delete_req | C→S | RoleHandler |
| `kCliRoleLoginReq/Res` | cli_role_login_* | C↔S | RoleHandler |
| `kCliRandomName/Res` | cli_random_name* | C↔S | RoleHandler |

### 21.3 进游戏 / 3D 视野 / 移动

| 宏 | 名称 | 方向 | 来源 |
|----|------|------|------|
| `kCliEnterGameReq/Res` | cli_enter_game_* | C↔S | GameHandler |
| `kCliGlobalConfigRes` | cli_global_config_res | S→C | GameHandler（进图后推） |
| `kCli3dEnterMap` | cli_3d_enter_map | S→C | AoiViewBridge::OnEntityEnterMap |
| `kCli3dEntityAppearEx` | cli_3d_aoi_appears_ntf | S→C | Bridge（含 NotifySelfAppear） |
| `kCli3dEntityDisappearEx` | cli_3d_entity_disappear_ex | S→C | Bridge leave |
| `kCli3dMoveReq/Res` | cli_3d_move_* | C↔S | MoveHandler |

> `kCli3dEntityAppearsEx` / `DisappearsEx` 等批量宏已定义，当前 Bridge 走单实体 Ex。

### 21.4 常见 err_code / op_code

| 协议 | 码 | 含义 |
|------|-----|------|
| user_login_res | 0 | 成功 |
| user_login_res | 1 | uid 为空 |
| user_login_res | 2 | 未握手 |
| reconnect_res | 0 | session 有效 |
| reconnect_res | 1 | session 无效/过期 |
| role_list_res.op_code | 1 | 查询列表 |
| role_list_res.op_code | 2 | 创建后回列表 |
| role_list_res.op_code | 3 | 删除后回列表 |
| role_delete 路径 | err=1 | 在游戏中禁止删角 / 状态不符 |
| enter_game_res | 0 | 成功 |
| enter_game_res | 1 | 未选角（状态不对） |
| enter_game_res | 2 | 无地图配置 |
| enter_game_res | 3 | 已在图中 |
| enter_game_res | 4 | 连接已断开 |
| enter_game_res | 5 | 无有效 role_id |
| move_res.success | 1/0 | 接受 / 拒绝 |
| kickoff code_id | 1/2/3 | 服务器踢人 / 顶号 / 顶角色 |
| handshake_res.code | 0 | 接受 |

常量定义见 `src/gameserver/server_constants.h`（`GAME_GATE_ADDR` 可覆盖 gate）。

---

---

## 24. 网络帧编解码

### 24.1 收包

```
TcpConnection 字节流
  → TryDecodeFrames(buf) → vector<PackFrame>
  → 失败：Shutdown（防协议污染）
  → 成功：逐帧 Handler
```

`PackFrame` 含：`flags`、`msg_id`、`recv_index`（序号）、`body`（protobuf）。

### 24.2 发包

```
SendMsg / SendFrame
  → flags = kPackFlagEncrypt
  → recv_index = (*send_seq_)++   // uint8 回绕可接受
  → EncodeFrame → conn->Send
```

### 24.3 Context

- 建连：`SetContext(EntityPtr)`  
- 登录/重连/role_list 恢复：换成缓存实体  
- 顶号踢旧：`SetContext({})` 防异步 disconnect 误伤  

---
