<!-- 拆自 CODE_FLOWS；权威目录 doc/server/ -->

## 25. 登录 / 顶号 / 选角逐步时序

### 25.1 首次登录

```
handshake → kHandshaked
user_login(uid) → OnUserLogin
  → AccountComponent + session_id
  → RegisterByUid/SessionId
  → kLoggedIn
  → login_res(session_id)
role_list / create / login
  → kRoleSelected
```

### 25.2 同 uid 重登（顶号）

```
新连接临时 entity 发 login
FindByUid → existing
  → 旧 conn：kickoff + SetContext{} + Shutdown
  → existing 挂新 ConnectionComponent
  → 新 GenSessionId；UnregisterBySessionId(旧)
  → Register 新 session；CleanupEntity(临时)
  → SetContext(existing)；状态 kLoggedIn
```

旧连接随后 disconnect：**stale 校验**跳过 LeaveMap。

### 25.3 role_list 带 session 恢复

与重连类似：校验 session → 顶旧连接 → 挂 cached → `kLoggedIn` → 回角色列表。  
用于 U3D 客户端「新 TCP 直接拉角色列表」路径。

### 25.4 选角顶号

同 `role_id` 已在线：**先校验请求方 uid 与持有方一致**，拒绝则直接 `err=1`（不 LeaveMap/不踢线）；通过后再 `LeaveMap(existing)` + kickoff + `kDisconnected`，最后 `OnRoleLogin` 当前连接。

---
