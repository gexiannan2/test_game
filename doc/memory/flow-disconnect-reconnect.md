# 记忆：断线 / 重连 / 踢人 / 优雅停机

**关键词**：disconnect、reconnect、stale、KickAndShutdownConnection、GracefulStop  
**最后扫描**：2026-08-05

---

## 断线调用链

```
TCP close / 心跳超时 / kick
  → OnConnection(false)
      → cc->conn_ != conn → 忽略（stale）
      → cc->conn_.reset()（防 AOI 再往死连接 Send）
      → if IsInMap: LeaveMap
      → RemoveComponent<ConnectionComponent>
      → FlushPlayer(role_id)（Mongo）
      → 实体留 PES 缓存（uid/session/role 索引）
```

---

## 重连调用链

```
cli_reconnect_req(session_id)
  → FindBySessionId + Account.session_id_ 二次校验
  → KickAndShutdownConnection(cached)
  → 绑 conn 到 cached + OnReconnect → kLoggedIn
  → if was_in_game (kInGame || IsInMap):
        LeaveMap → kInGame → EnterMap（重建双向视野）
  → CleanupEntity(temp) + SetContext(cached)
```

**注意**：用 `kInGame || IsInMap` 判断，勿用 `HasComponent<MapComponent>`。

---

## 踢线

`KickAndShutdownConnection(entity, code, reason, replacing_conn)`  
顺序：LeaveMap（调用方）→ kickoff ntf → SetContext({}) → Shutdown 旧 conn

---

## 优雅停机

```
SIGINT → DoGracefulStop
  → 取消定时器
  → 所有 IsInMap 玩家 LeaveMap（disappear 可发出）
  → FlushOnShutdown（Mongo）
  → server_.Stop → ForceClose → loop_.Quit
```

---

## 关键文件

| 文件 | 职责 |
|------|------|
| `game_server.cpp` | OnConnection、CheckHeartbeatTimeout、DoGracefulStop |
| `handlers/connection_handler.cpp` | reconnect 协议 |
| `session/connection_system.cpp` | OnReconnect、OnKickoff |

---

## 硬约束

1. stale disconnect 不得 LeaveMap  
2. 心跳超时须 **FlushPlayer**  
3. LeaveMap 后 **ClearPropertyTypes**  
4. reconnect/enter_game：**IsInMap 先 LeaveMap 再 EnterMap**

---

## 相关测试

- `LoginAoiChurnTest`  
- `EdgeCaseTest::AoiLeaveCallbackNestedLeaveMapSafe`  
- `client/protocol_test.cc`（reconnect 路径）
