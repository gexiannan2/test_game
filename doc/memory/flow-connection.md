# 记忆：连接 / 握手 / 收包

**关键词**：connect、handshake、heartbeat、Context、粘包、send_seq  
**最后扫描**：2026-08-05

---

## 调用链

```
TCP connect
  → GameServer::OnConnection(true)
      → PlayerEntity(AllocateEntityId) + ConnectionComponent
      → conn->SetContext(entity)

每帧收包
  → OnMessage → pack_codec Decode → 刷新 Context entity（防粘包用旧实体）
  → FindHandler(msg_id) → IHandler::Handle

cli_handshake_req
  → ConnectionHandler → System::OnHandshake → kHandshaked
  → cli_handshake_res

cli_heart_beat_req
  → System::OnHeartbeat（更新 last_heartbeat_sec_）
  → cli_heart_beat_res(server_time)

心跳超时
  → CheckHeartbeatTimeout → LeaveMap + FlushPlayer + kick
```

---

## 关键文件

| 文件 | 职责 |
|------|------|
| `src/gameserver/game_server.cpp` | OnConnection、OnMessage、CheckHeartbeatTimeout |
| `src/gameserver/handlers/connection_handler.cpp` | 握手/心跳/重连协议 |
| `src/gameserver/session/connection_system.cpp` | OnHandshake/OnHeartbeat/OnReconnect/OnKickoff |
| `src/protocol/pack_codec.cpp` | 帧编解码 |
| `src/gameserver/ecs/components/connection_component.h` | conn_、send_seq_、last_heartbeat_sec_ |

---

## 硬约束

1. **粘包**：每帧 `OnMessage` 必须从 `conn->GetContext()` 取最新 entity  
2. **stale disconnect**：`cc->conn_ != conn` 时忽略离图（顶号/重连后旧连接异步断开）  
3. **拒绝须回包**：cast 失败也要发 `*_res`（2026-08-05 已修 handshake/reconnect/login/role_list）  
4. **OnKickoff 不改 IsInMap**：调用方先 `LeaveMap`  
5. **顶号踢线**：`KickAndShutdownConnection` 前 `SetContext({})`，防旧 disconnect 误操作

---

## 实体状态

`kConnected` → `kHandshaked` → `kLoggedIn` → `kRoleSelected` → `kInGame` / `kDisconnected`

---

## 相关测试

- `HandlerLogicTest`（无网络）  
- `client/protocol_test.cc`（E2E 握手链）
