<!-- 拆自 CODE_FLOWS；权威目录 doc/server/ -->

## 30. 客户端联调检查清单

### 30.1 最小闭环

1. TCP 连 `127.0.0.1:20002`（或启动参数）  
2. handshake → login(uid 非空) → 保存 session_id  
3. role_list →（可选 create）→ role_login  
4. enter_game → 应收：enter_game_res(0)、global_config、**enter_map**、**自身 appear**、**周围 appear**  
5. 周期 heart_beat（&lt;30s）  
6. move_req → 自己收到 move_res；跨约 10m 后邻居应有 appear/disappear  

### 30.2 多客户端

- A、B 同 AOI 格互进：互相收到对方 appear  
- A 走出 B 的 10m 格：互相 disappear  
- A 重登顶号：B 先 disappear 再（若 A 再进图）appear；A 旧端收到 kickoff  

### 30.3 重连

- 断线后新 TCP + reconnect(session)  
- 成功：若曾进游戏，应再次 enter_map + 视野重建 + 自身 appear  
- 失败 err=1：重新走 login  

### 30.4 服务端日志关键字

`[RECV]` / `[SEND]` / `entered game` / `reconnect` / `kickoff` / `heartbeat timeout` / `stale disconnect`

---

*文档版本：§1–§20 初版 + §21–§30 续写。与仓库 `src/gameserver` 实现同步；冲突以代码为准并回写本文。*
