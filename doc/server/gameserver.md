<!-- 拆自 CODE_FLOWS；权威目录 doc/server/ -->

## 3. GameServer 编排层

**文件**：`game_server.h` / `game_server.cpp`  
**入口**：`main.cc` → `GameServer(ip,port)` → `Start()` → `Loop()`

### 3.1 Start() 顺序（敏感）

1. `MapConfigSystem::LoadDefaults()`
2. `PlayerConfig::LoadDefaults()`（胶囊 height=1.5, radius=0.3）
3. `WorldSystem::Create(kMap)` + `Init()`
4. 构造 `AoiViewBridge(world, SendFn)` + `Install()`  
   - SendFn：`FindEntity(conn_owner_id)` → `ConnectionComponent` → `SendFrame`
5. `RegisterHandlerMulti`：Connection / Account / Role / Game / Move
6. `TcpServer`：`SetThreadNum(0)`（单线程），绑 `OnConnection`/`OnMessage`，`Start()`
7. 心跳定时器：每 **5s** → `CheckHeartbeatTimeout`（超时 **30s**）
8. 世界 Tick 定时器：约 **1/60s** → `world_->Tick()`

### 3.2 OnConnection

**建连**

- `AllocateEntityId()` → `PlayerEntity`
- `AddComponent<ConnectionComponent>`，`conn_` 赋值
- `conn->SetContext(EntityPtr)`，状态默认 `kConnected`

**断线**

- **Stale 防护**：若 `ConnectionComponent::conn_ != 当前断开的 conn`，说明实体已迁移到新连接，直接 return（防顶号竞态误 LeaveMap）
- `SetState(kDisconnected)`
- `IsInMap()` → `LeaveMap` + `SetInMap(false)`
- `RemoveComponent<ConnectionComponent>`（账号/角色/Transform/Map **保留**在 PES）

### 3.3 OnMessage

1. `TryDecodeFrames` 失败 → `Shutdown`
2. **每帧**重新 `any_cast` Context 取 entity（粘包首帧可能 `SetContext` 换实体）
3. `FindHandler(msg_id)` → `Handle(conn, entity, frame)`；未知 msg_id 只告警

### 3.4 发包

- `SendFrame`：`kPackFlagEncrypt` + msg_id + 递增 `send_seq_` + body → `EncodeFrame` → `Send`
- `SendMsg<T>`：protobuf `SerializeToString` 后调 `SendFrame`
- `GenSessionId` 起点 **10001**；`GenRoleId` 起点 **20001**

### 3.5 CheckHeartbeatTimeout

- 扫描 `PlayerEntitySystem::GetAllByUidSnapshot`
- 跳过无连接、`kConnected`、`kDisconnected`
- `last_heartbeat_sec_ == 0` 不踢（避免刚握手误杀）
- 超时：`LeaveMap`（若在图）→ `kDisconnected` → 清空旧 conn Context → `RemoveComponent<Connection>` → `Shutdown`

---
