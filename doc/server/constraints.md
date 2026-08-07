<!-- 拆自 CODE_FLOWS；权威目录 doc/server/ -->

## 18. 设计约束与已知坑

1. **MoveEntity 必须在 OnMove 之前**  
2. **自身 appear 仅 `NotifySelfAppear` 一次**（`EnterMap` 末尾）；update/disappear 永不推自身  
3. **进图时周围人由 AddWatcher 推给自己** —— 勿重复实现  
4. **kInGame ≠ IsInMap**  
5. **顶号必须清旧 Context**，断线校验 conn 归属  
6. **粘包每帧刷新 entity Context**  
7. **LeaveMap / MarkPropertyDirty 幂等与在图守卫**（LeaveMap 须 `ClearPropertyTypes`，防移动中离图残留 `kStopMove`）  
8. **脏跨格勿双重 kUpdate**（即时推送后清脏位）  
9. **SerializeDirty 须含 move_rot**  
10. **BroadcastToMap 已废弃**，视野只走 AOI Bridge  
11. **zrpc 写/读对齐 muduo**：`SendInLoop`/`HandleWrite` 写失败不 `HandleClose`；`HandleRead` 仅 `n==0` 关连接，读错误只 `HandleError`；缓冲超限放弃写/读，不关连接。关连接靠 read0 / POLLHUP / 应用 `ForceClose`。  
12. **同实体再登录须先 LeaveMap**：`OnUserLogin` 会降为 `kLoggedIn`，若仍 `IsInMap` 则 move/enter_game 软锁；同时须 `UnregisterBySessionId(旧)`。  
13. **enter_game 与 reconnect 一样**：`IsInMap` 时先 `LeaveMap` 再 `EnterMap`。  
14. **顶号/重绑 ConnectionComponent 勿 Emplace 重置**：有组件则原地改 `conn_`，保留 `send_seq_`。  
15. **role_login 踢人前先校验**：跨账号伪造 `role_id` 不得 LeaveMap/kick 受害者；统一走 `System::CheckRoleLogin`，通过后再踢。  
16. **AOI 不推自身 update/disappear**：与 appear 同样过滤 `watcher_id == subject_id`。  
17. **OnKickoff 不改 IsInMap**：调用方先 `LeaveMap`，避免只清标志留下 AOI 幽灵。  
18. **拒绝类请求须回包**：role/move/enter_game 状态不符或校验失败发对应 `*_res`，禁止静默 `return`。  

---
