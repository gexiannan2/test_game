<!-- 拆自 CODE_FLOWS；权威目录 doc/server/ -->

## 16. 离图 / 断线 / 心跳 / 重连

### 16.1 离图触发源

- TCP `OnConnection(!Connected)`（含 stale 防护）  
- 心跳超时  
- `kCliKickoffPlayer`  
- 重连前若仍 `IsInMap` 先 Leave 再 Enter  
- 同 role 选角顶号 `LeaveMap(existing)`  

### 16.2 重连要点

1. session 索引命中 **且** `Account.session_id_` 一致  
2. 踢旧连接前 `SetContext({})`  
3. 有 `MapComponent` 则恢复 `kInGame` + EnterMap（双向视野重建）+ 自身 appear  

### 16.3 重登 session

每次登录 `GenSessionId`，`UnregisterBySessionId(旧)`，旧设备无法用旧 session 重连。

---
