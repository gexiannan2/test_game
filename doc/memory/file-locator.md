# 记忆：文件定位器（改 X 看哪里）

**最后扫描**：2026-08-05

---

## 按问题定位

| 我想… | 先看 | 再看 |
|-------|------|------|
| 改协议处理 | `handlers/*_handler.cpp` | `session/*_system.cpp` |
| 改进图/AOI | `ecs/systems/world_system.cpp` | `aoi_system.cpp` / `aoi_sector.cpp` |
| 改视野推包 | `aoi_view_bridge.cpp` | `player_entity.cpp` Serialize* |
| 改移动/跨格 | `move_handler.cpp` | `world_system.cpp::MoveEntity` |
| 改 NPC 寻路 | `move_system.cpp` | `move_component.cpp` |
| 改地图格 | `map_system.cpp` | `map_grid.cpp` |
| 改物理/边界 | `jolt_server.cpp` | `jolt_system.cpp` / `map_bounds_util.h` |
| 改登录顶号 | `account_handler.cpp` | `account_system.cpp` |
| 改选角 | `role_handler.cpp` | `role_system.cpp` |
| 改重连 | `connection_handler.cpp` | `game_server.cpp::OnConnection` |
| 改心跳/踢人 | `game_server.cpp` | `connection_system.cpp` |
| 改落库 | `player_persist_system.cpp` | `mongo/PlayerMongoStorage.*` |
| 改帧格式 | `protocol/pack_codec.cpp` | `protocol/pack_flags.h` |
| 改常量/err | `common/server_constants.h` | proto 定义 |
| 改坐标 | `common/vector3d.h` | `common/aoi_def.h` |
| 加回归测试 | `server/tests/*.cc` | `test_harness.h` |
| E2E 协议测 | `client/protocol_test.cc` | `client/test_client.h` |
| AOI 进出视野 E2E | `client/aoi_enter_leave_test.cc` | `client/test_client.h` |
| AOI Oracle 对拍 | `server/tests/test_aoi_oracle.h` | `aoi_parity_test.cc` |

---

## 目录职责（勿混）

| 路径 | 职责 | 禁止 |
|------|------|------|
| `session/` | 账号/连接/角色 System:: | AOI/物理/移动仿真 |
| `ecs/systems/` | World/Map/AOI/Move/Jolt/Persist | 登录协议 |
| `handlers/` | 协议入口编排 | 大块 AOI 算法 |
| `mongo/` | 异步落库 | 状态机 |
| `server/systems/` | **遗留副本，未进 CMake** | 勿改 |

---

## 入口点

| 目标 | 文件 |
|------|------|
| 服务端 main | `src/gameserver/main.cc` |
| 客户端联调 | `src/client/main.cc` |
| 压测 | `src/client/stress_test.cc` |
| 单元测试入口 | `src/gameserver/tests/test_main.cc` |

---

## 详细矩阵

完整版：`doc/server/file-matrix.md`
