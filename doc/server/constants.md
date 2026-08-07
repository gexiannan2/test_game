<!-- 拆自 CODE_FLOWS；权威目录 doc/server/ -->

## 17. 常量与 ID 空间

| 项 | 值 |
|----|-----|
| 地图逻辑格 | 4m |
| AOI 格 | 10m |
| 观察邻域半径 | 0（单格） |
| 心跳扫描 / 超时 | 5s / 30s |
| entity_id | 从 1 递增 |
| session_id | 从 10001 |
| role_id（协议 ins_id） | 从 20001 |
| 默认 map_ins_id | 1（`server::kDefaultMapInstanceId`） |
| 默认地图 cfg | 1001 |
| 默认 gate | `10.23.0.99:20002`（`GAME_GATE_ADDR` 可覆盖） |
| 协议 err / kickoff | 见 `src/gameserver/server_constants.h` |

---
