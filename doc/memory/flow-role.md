# 记忆：角色列表 / 选角 / CRUD

**关键词**：role_list、role_login、role_create、CheckRoleLogin、TransferGameplayComponents  
**最后扫描**：2026-08-05

---

## 调用链

```
cli_role_list_req
  → [可选] session_id 恢复 cached 实体（顶旧连接 + LeaveMap + kLoggedIn）
  → System::OnRoleListReq
  → cli_role_list_res

cli_role_create_req → OnRoleCreate → RegisterByRoleId
cli_role_delete_req → OnRoleDelete → Unregister(role_id) + DeleteRoleArchive(Mongo)

cli_role_login_req
  → CheckRoleLogin（跨账号 role_id → kDeniedUid，**不踢人**）
  → [通过] FindByRoleId(existing) → LeaveMap + kick + kDisconnected
  → OnRoleLogin → TransferGameplayComponentsFrom(cached) + kRoleSelected
  → cli_role_login_res
```

---

## 关键文件

| 文件 | 职责 |
|------|------|
| `handlers/role_handler.cpp` | 协议编排、session 恢复、踢人顺序 |
| `session/role_system.cpp` | CRUD、CheckRoleLogin、OnRoleLogin |
| `ecs/entity/entity.cpp` | TransferGameplayComponentsFrom |
| `session_lifecycle.h` | TransferGameplayComponents 工具 |

---

## 硬约束

1. **role_login 踢人前必须 CheckRoleLogin 通过**（防跨账号伪造 role_id 踢受害者）  
2. **role_list session 恢复**：须 LeaveMap + 保留 send_seq_（原地改 conn_，勿 Emplace 新组件）  
3. **OnRoleListReq uid 不一致**：只复制 RoleComponent，不 RegisterByRoleId  
4. **拒绝须回包**：状态不符 / cast 失败 → 对应 `*_res`  
5. **kInGame 不可删角色**

---

## 索引不变量

- `CleanupEntity` 仅擦除仍指向本实体的 uid/session/role 项  
- 删光角色后 uid 索引保留，role_id 索引清空  
- `is_last` 至多一个（见 `RecursiveScanTest`）

---

## 相关测试

- `HandlerLogicTest`  
- `RecursiveScanTest`  
- `LoginAoiChurnTest`（顶角色组件迁移）
