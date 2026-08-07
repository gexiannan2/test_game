# 记忆：登录 / 顶号 / Session

**关键词**：login、uid、session_id、顶号、FindByUid、UnregisterBySessionId  
**最后扫描**：2026-08-05

---

## 调用链

### 首次登录（同连接）

```
cli_user_login_req
  → AccountHandler
  → System::OnUserLogin(uid, token, channel, GenSessionId)
      → AccountComponent + RegisterByUid/SessionId
      → kLoggedIn
  → cli_user_login_res(session_id, gate_addr)
```

### 同 uid 顶号（新连接 + 旧缓存实体）

```
FindByUid → existing != temp_entity
  → existing.IsInMap → LeaveMap(existing)   // 必须先离图
  → KickAndShutdownConnection(existing)
  → 挂新 ConnectionComponent 到 existing
  → OnUserLogin → UnregisterBySessionId(旧session)
  → CleanupEntity(temp) + SetContext(existing)
  → login_res
```

### 同实体再登录（同连接重复 login）

```
AccountHandler: entity->IsInMap() → LeaveMap   // 防 kLoggedIn+IsInMap 软锁
→ OnUserLogin（内部 UnregisterBySessionId 旧 session）
```

---

## 关键文件

| 文件 | 职责 |
|------|------|
| `handlers/account_handler.cpp` | 顶号编排、LeaveMap 守卫 |
| `session/account_system.cpp` | OnUserLogin、session 轮换 |
| `ecs/systems/player_entity_system.*` | uid/session_id/role_id 索引 |
| `common/server_constants.h` | kLoginErr*、kKickoffReplaceAccount |

---

## err_code

| 值 | 常量 | 含义 |
|----|------|------|
| 0 | kLoginErrOk | 成功 |
| 1 | kLoginErrEmptyUid | uid 空 |
| 2 | kLoginErrNeedHandshake | 未握手 |
| 3 | kLoginErrBadRequest | 解码/非法请求 |

---

## 硬约束

1. 每次登录 **GenSessionId**，旧 session 失效  
2. 顶号顺序：**LeaveMap → kick → 绑新 conn**  
3. **token 校验**：当前开发态，未做权威校验（已知项）  
4. cast 失败必须 `cli_user_login_res(err≠0)`

---

## 相关测试

- `LoginAoiChurnTest`  
- `HandlerLogicTest`  
- `client/protocol_regression_test.cc`
