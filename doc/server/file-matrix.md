<!-- 拆自 CODE_FLOWS；权威目录 doc/server/ -->

## 23. 源码文件职责矩阵

| 文件 | 一句话职责 | 常改场景 |
|------|------------|----------|
| `main.cc` | 进程入口、默认端口 20002 | 启动参数 |
| `game_server.*` | TCP、注册、心跳、Tick、Context；`KickAndShutdownConnection` / `EnterMapAndPushSelfAppear` | 连接生命周期 |
| `server_constants.h` | err_code / kickoff / gate / 默认 map_ins | 去硬编码 |
| `handlers/connection_handler.*` | 握手/心跳/重连/踢人 | 重连恢复 |
| `handlers/account_handler.*` | 登录/重登顶号 | session 轮换 |
| `handlers/role_handler.*` | 角色 CRUD/选角；`CheckRoleLogin` 后再踢 | 状态机门禁 |
| `handlers/game_handler.*` | enter_game、自身 appear | 出生点/进图 |
| `handlers/move_handler.*` | move_req、MoveEntity 顺序；拒绝回包 | 移动同步 |
| `mongo/*` | 玩家快照异步写 Mongo（`PlayerMongoStorage`） | 持久化接入 |
| `systems/world_system.*` | Enter/Leave/Move 编排 | 场景总线 |
| `systems/map_system.*` | 4m 稀疏格足迹 | 占地/回调 |
| `systems/map_grid.*` | 单格实体集合 | 格内查询 |
| `systems/aoi_system.*` | Watcher 表、脏刷、多 Sector | 视野 API |
| `systems/aoi_sector.*` | Cell/Monitor/SwitchMonitor | 跨格算法 |
| `systems/aoi_view_bridge.*` | 事件→protobuf | 协议字段 |
| `systems/aoi_def.h` | 枚举/常量/回调类型 | AOI 参数 |
| `systems/move_system.*` | NPC 路径 Tick | 服务端移动 |
| `session/system.h` + `*_system.cpp` | 登录/会话/角色门面（原 `server/systems/`） | 账号角色 |
| `map_bounds_util.h` / `session_lifecycle.h` | 边界钳制、顶号组件迁移 | 共用逻辑 |
| `ecs/entity/entity.*` | 基类、脏属性、格中心 | 状态/Transform 代理 |
| `ecs/entity/player_entity.*` | Appear/Dirty 序列化 | 协议字段 |
| `ecs/components/*` | 各组件字段 | 数据扩展 |
| `ecs/components/move_component.*` | 路径积分 → MoveEntity | NPC 移动 |
| `tests/*` | 回归/对拍/压测 | 行为锁 |

---
