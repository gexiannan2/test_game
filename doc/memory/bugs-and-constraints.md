# 记忆：硬约束 + 历史 Bug 速查

**最后扫描**：2026-08-05 | 全量测试 **127/127** 通过

---

## P0 硬约束（违反 → 软锁 / 崩溃 / 幽灵实体）

| # | 约束 | 典型症状 |
|---|------|----------|
| 1 | MoveEntity **在** SetMoveState **之后** | Jolt 读旧 rot，跨格漏广播 |
| 2 | 进图前 IsInMap → **先 LeaveMap** | EnterMap 短路，AOI 不同步 |
| 3 | 顶号/kick：**LeaveMap → kick** | AOI 绑死连接、幽灵 |
| 4 | 断线校验 `cc->conn_ == conn` | 旧连接误 LeaveMap |
| 5 | 粘包每帧刷新 Context entity | 多帧用临时实体 |
| 6 | 自身 appear **仅** `NotifySelfAppear` 一次；update/disappear **永不推自身** | 重复包 / 幽灵叠层 |
| 7 | 同格移动须 **SetPropertyDirty(kMove)** | 邻居看不到 update |
| 8 | LeaveMap 后 **ClearPropertyTypes** | kStopMove 残留 |
| 9 | persist 入队前 **ClearDirty** | Mongo 写风暴 |
| 10 | CleanupEntity **条件擦除**索引 | 误删新连接 uid |
| 11 | 多层 FlushDirty：**推完再清脏** | 远景丢 update |
| 12 | 拒绝类请求 **必须回 *_res** | 客户端挂起 |

---

## 2026-08-05 新修

| 级别 | 问题 | 修复 |
|------|------|------|
| P2 | account/connection/role handler cast 失败静默丢包 | 补发 login/handshake/reconnect/role_list res |

新增常量：`kLoginErrBadRequest = 3`（`server_constants.h`）

---

## 历史 P0/P1（已修，回归时注意）

详见 `doc/server/changelog-bugs.md` 与 `doc/server/记忆.md` §3。

高频回归点：

- reconnect/enter_game **IsInMap 残留**  
- 同格移动 **无 AOI 脏位**  
- role_login **校验前踢人**（跨账号漏洞）  
- zrpc Write 失败 **同步 HandleClose** → AOI 重入崩  
- 顶号 **Channel::Remove 断言**（zrpc kDisconnecting）

---

## 已知风险（AOI，未修）

| 项 | 触发 | 症状 |
|----|------|------|
| MoveWatcher 自愈 | `MoveWatcher` 失败 → `RemoveWatcher(false)` + `AddWatcher` | 邻居可能收到重复 appear |
| `HasWatcher` 只查中心格 | sector 与 `watchers_` 失步 | 偶发跳过 MoveWatcher / 重复 AddWatcher |

---

## 已知未接线

- enter_game 未 QueryRole → 重启出生点  
- token/session 权威校验（开发态）  
- Snappy ZIPS 未实现（pack_codec 拒绝）

---

## 改代码 MR 自查

- [ ] 进图路径处理 IsInMap 残留？  
- [ ] 断线/超时/kick 都 FlushPlayer？  
- [ ] 同格移动标 AOI dirty？  
- [ ] 拒绝路径有回包？  
- [ ] 跑 `./bin/svc_game_3d_test`（至少 LoginAoiChurn + Regression）
