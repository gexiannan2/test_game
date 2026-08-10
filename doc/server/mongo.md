# 玩家数据 MongoDB 异步落地模块

> 状态：已接入构建（mongo 总是编译，无条件编译开关）+ 移动异步落地已实现（节流 + PostSave + shutdown 排空）；单元/集成测试待补  
> 源码目标路径：`src/mongo/`  
> 现有原型：已迁入（原 `MongoStandalone`）  
> 驱动：mongo-c-driver **2.3.3** + mongo-cxx-driver **r4.4.1**（源码静态编入，不随服发布 DLL/SO）

---

## 1. 背景与目标

### 1.1 现状

- `MongoStandalone` 已是可独立构建的玩家快照异步持久化原型，原 CMake 目标名为 `mongo_player_storage`。
- 核心能力已具备：
  - `PlayerMongoStorage::PostSave(PlayerSnapshot)` — 值语义入队
  - `AsyncMongoDispatcher` — 按 `playerId` 固定到 worker，同玩家 FIFO
  - `MongoClient` — 连接池 + CRUD
  - 模块自带 CRUD / 异步派发 / QPS 基准测试
- 游戏服 `src/gameserver` 尚未引用该模块；移动路径（`MoveHandler`）只改内存 AOI，无持久化。

### 1.2 本阶段目标（MVP）

1. **重命名并接入构建**：目录改为 `mongo`，根 `CMakeLists.txt` 成功链上模块，游戏服与相关测试可编译。
2. **移动异步落地**：玩家 `cli_3d_move_req` 成功更新世界状态后，异步 upsert 位置等到 MongoDB。
3. **单元 / 集成测试**：覆盖快照组装、投递、落地可读回；Mongo 不可用时测试可跳过（环境门控）。
4. **文档同步**：本文件 + `README.md` / `architecture.md` / `flows-move.md` / `file-matrix.md` / `testing.md`。

### 1.3 非目标（本阶段不做）

- 登录 / 选角 / 进图时从 MongoDB 加载完整存档。
- 技能、属性、背包等全量字段完备化（快照结构先落库，字段可逐步填）。
- 跨服副本集、TLS、SASL 生产加固（保留 `MongoConfig` 能力，默认内网 `directConnection`）。
- 跨服副本集、TLS 生产加固以外的运维细节。

---

## 2. 命名与目录

| 项 | 旧 | 新 |
|----|----|----|
| 目录 | `src/mongoStandalone/MongoStandalone/` | `src/mongo/` |
| CMake 目标 | `mongo_player_storage` | `mongo` |
| include / 命名空间 | `mongo_standalone` | `mongo` |
| 头文件 | `mongo_standalone/...` | `src/mongo/include/*.h`（`#include "PlayerMongoStorage.h"`） |
| 驱动源码 | 模块内 `third_party/src/` | 仓库根 `third_party/mongo-{c,cxx}-driver` |

外层空壳 `MongoStandalone/` 删除；`.git` / `.vs` / `build` / `out` / `results` 等本地产物不进入主仓构建图（`.gitignore` 覆盖）。

模块内原 option `MONGO_PLAYER_STORAGE_BUILD_TESTS` 改为 `MONGO_BUILD_TESTS`。

---

## 3. 架构位置

```
MoveHandler / (可选) WorldSystem
        │  值拷贝快照（不跨线程持有 Entity*）
        ▼
PlayerPersistFacade（游戏服薄封装，可选）
        │  PostSave / 节流
        ▼
mongo::PlayerMongoStorage
        │  按 playerId 路由
        ▼
AsyncMongoDispatcher workers
        │
        ▼
MongoClient pool → MongoDB (players collection)
```

分层约束：

- **Handler / World 主线程**：只做「组快照 + PostSave」；禁止在主线程同步写库。
- **Worker 线程**：只碰 `MongoClient` 与值语义快照；禁止回调游戏主循环改 Entity。
- **生命周期**：`GameServer::Start` 创建并持有；`Stop` 先停收包/`RequestStop`，再 `WaitForIdle`，最后 `Stop` 回收 worker。

与现有铁律无关但需并列记住：

- 移动仍是 **先 `MoveEntity` 再 `OnMove`**；持久化挂在二者成功之后、回包前后均可（推荐在 `OnMove` 之后，保证 Transform 已一致）。

---

## 4. 数据模型

集合默认：`players`（可用 `PlayerMongoStorageOptions::collection` / 环境变量覆盖）。

文档（与现有 `PlayerMongoStorage::SaveSnapshot` 对齐）：

```json
{
  "_id": 20001,
  "name": "hero",
  "level": 1,
  "position": { "map_id": 1, "x": 10.0, "y": 0.0, "z": 20.0 },
  "attributes": { "hp": 0, "mp": 0, "attack": 0 },
  "skills": [],
  "settings": { "auto_pick": false },
  "last_sequence": 42
}
```

### 4.1 快照字段映射（移动路径）

| `PlayerSnapshot` | 来源 |
|------------------|------|
| `id` | `RoleComponent::role_id_`（无则 `entity->GetId()`） |
| `name` / `level` | `RoleComponent` |
| `position.mapId` | `MapComponent::map_cfg_id_`（cast 到 int32，超限打日志并钳制/失败跳过） |
| `position.x/y/z` | `TransformComponent::pos_` |
| `sequence` | 每角色单调递增（进程内原子或组件字段）；用于乱序覆盖可观测 |
| attributes / skills / autoPick | MVP 填默认；后续登录落地再补 |

写入语义：`UpdateOne(..., upsert=true)` + `$set`。同玩家 FIFO 保证「后入队覆盖先生效」；高频移动下仍可能有中间态落库，可接受。

---

## 5. 移动接入设计

### 5.1 挂点

文件：`handlers/move_handler.cpp`

```
校验通过
  → MoveEntity(pos)
  → OnMove(...)
  → PersistPlayerAfterMove(entity)   // 新增：节流 + PostSave
  → SendMoveRes(success)
```

不改 `MoveEntity` 内部，避免 NPC / 压力测试路径被迫打库；NPC 路径需要时可后续显式调用。

### 5.2 节流（必做）

移动包可达数十 Hz；若每包 `PostSave` 会打满队列与磁盘。

建议默认：

| 参数 | 默认 | 说明 |
|------|------|------|
| `min_interval_ms` | `500` | 同一 `role_id` 两次成功入队最小间隔 |
| 环境变量 | `GAME_MONGO_MOVE_PERSIST_MS` | `0` 表示关闭移动落地 |

节流实现建议放在游戏服侧薄封装（例如 `systems/player_persist_system.*`），模块本身保持通用 `PostSave`。

可选增强（MVP 可不做）：停服 / 离图时强制 flush 一次最新坐标。

### 5.3 失败策略

| 情况 | 行为 |
|------|------|
| Mongo 未启用 / 未初始化 | 静默跳过（日志一次性 WARN） |
| 队列满 `PostSave==false` | WARN + metrics.rejected；不阻塞移动回包 |
| Worker 写库异常 | `ErrorHandler` 打日志；不回滚内存位置 |
| 关服超时未排空 | ERROR 日志；允许进程退出（接受最后窗口丢写） |

**原则**：持久化失败不得影响战斗/AOI/移动正确性。

### 5.4 GameServer 持有方式

```cpp
// game_server.h
std::unique_ptr<mongo::PlayerMongoStorage> player_storage_;

mongo::PlayerMongoStorage* GetPlayerStorage() { return player_storage_.get(); }
```

启动：

- 环境变量 `GAME_MONGO_ENABLE=1`（或 `MONGO_URI` 已设置）时初始化；否则不建对象。
- 配置来自 `MongoConfig::FromEnvironment()`。

停止（`GameServer::Stop` 路径）：

1. `RequestStop()`（拒新任务）
2. `WaitForIdle(3s~10s)`
3. `Stop()` / 析构

---

## 6. CMake 接入

根 `CMakeLists.txt`：

```cmake
option(GAME_ENABLE_MONGO "接入玩家 Mongo 异步落地" ON)

if(GAME_ENABLE_MONGO)
  set(MONGO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  add_subdirectory("${SRC_DIR}/server/mongo")
  list(APPEND INC_DIRS "${SRC_DIR}/server/mongo/include")
endif()

# svc_game_3d_server / 需要落地的测试目标：
if(GAME_ENABLE_MONGO)
  target_link_libraries(svc_game_3d_server PRIVATE mongo)
  target_compile_definitions(svc_game_3d_server PRIVATE GAME_HAS_MONGO=1)
endif()
```

说明：

- `mongo` 是 **INTERFACE**：业务 `.cpp` 编进最终可执行文件；驱动静态目标 `mongocxx_static` / `mongoc_static` 仅构建期链接。
- 默认关闭模块内上游测试；独立验证时：

```bash
cmake -S src/mongo -B build/mongo -DMONGO_BUILD_TESTS=ON
```

- 主工程仍以 **WSL Linux + CMake + make/Ninja** 为准（与现有服一致）。Windows 下的 Mongo **服务端进程**仅作运行依赖，不参与编译。

可选关闭：`-DGAME_ENABLE_MONGO=OFF` 用于无 Mongo 工具链的快速编译。

---

## 7. MongoDB 运行时（本机）

安装目录（用户提供）：

`D:\Users\admin\Documents\youduqt\16100713-100242-gexiannan\file\mongodb-windows-x86_64-8.3.7\mongodb-win32-x86_64-windows-8.3.7\bin`

### 7.1 启动示例（Windows）

```bat
mkdir D:\mongo-data
"%MONGO_BIN%\mongod.exe" --dbpath D:\mongo-data --bind_ip 127.0.0.1 --port 27017
```

WSL 内进程连 Windows 上的 mongod：优先 `localhost:27017`（WSL2 mirrored / localhost 转发）；若不通则用 Windows 主机 IP。

默认 URI（已与模块一致）：

`mongodb://127.0.0.1:27017/?directConnection=true`  
库名默认：`game`

### 7.2 环境变量

| 变量 | 用途 |
|------|------|
| `MONGO_URI` / `MONGO_DATABASE` | 连接与库名 |
| `GAME_MONGO_ENABLE` | 游戏服是否启用落地 |
| `GAME_MONGO_MOVE_PERSIST_MS` | 移动节流；`0` 关闭 |
| 模块自带 `MONGO_*` | 池大小、超时、写关注等（见 `MongoConfig::FromEnvironment`） |

---

## 8. 测试计划

### 8.1 模块原有测试（接入后仍可独立跑）

| 目标 | 内容 |
|------|------|
| `mongo_tests` | CRUD |
| `mongo_async_dispatch_tests` | 亲和性 + `PlayerMongoStorage` |
| `mongo_player_benchmark` | QPS（人工） |

门控：无 mongod 时跳过或 fail-fast 并打印启动提示。

### 8.2 游戏服新增测试

新增：`src/gameserver/tests/player_mongo_persist_test.cc`  
目标：挂到 `svc_game_3d_test`（或独立 `svc_game_3d_mongo_test`，避免默认 CI 强依赖 mongod）。

推荐用例：

1. **BuildSnapshotFromEntity** — 无 Mongo：从 Entity 组件组装 `PlayerSnapshot` 字段正确。
2. **PostSaveAfterMove_RoundTrip** — 启 mongod：模拟移动后 `PostSave`，`WaitForIdle`，`FindOne` 校验 `position` / `last_sequence`。
3. **ThrottleDropsBurst** — 短间隔多次移动，入队次数 ≤ 节流预期。
4. **DisabledWhenEnvOff** — `GAME_MONGO_ENABLE` 未开时移动不崩溃、不投递。

环境门控：`GAME_MONGO_TEST=1` 才跑需真实 Mongo 的用例（对齐 `aoi_mass_10k` 风格）。

### 8.3 手工验证

1. 启动 mongod → 启动 `svc_game_3d_server`（`GAME_MONGO_ENABLE=1`）。
2. 客户端移动若干秒。
3. `mongosh` 查询 `db.players.find({_id: <role_id>})` 看到坐标更新。

---

## 9. 实现步骤（确认后编码）

| 步 | 内容 | 验收 |
|----|------|------|
| A | 目录重命名为 `mongo` + include/namespace 对齐 + 清理嵌套壳 | 路径与头文件一致 |
| B | 根 CMake `add_subdirectory` + link server | WSL 编译 `svc_game_3d_server` 成功 |
| C | `PlayerPersistSystem`（节流）+ `GameServer` 生命周期 | 启停无泄漏、无死锁 |
| D | `MoveHandler` 挂载 `PostSave` | 移动不阻塞；日志可见投递 |
| E | 新增 `player_mongo_persist_test` + 文档回写 | 门控测试通过 |
| F | （可选）文档脚本：mongod 启动说明写入 `doc/server/faq.md` | 可复现 |

每步小改动、小验证；不顺手大范围格式化。

---

## 10. 风险与约束

| 风险 | 等级 | 缓解 |
|------|------|------|
| 驱动源码编进服，首次编译慢 | 中 | `EXCLUDE_FROM_ALL` + 增量缓存；可选 CMake OFF |
| 移动频率打爆队列 | 高 | 节流；队列满只打日志 |
| Worker 持有 `Entity*` | 高 | **禁止**；只传 `PlayerSnapshot` 值 |
| WSL ↔ Windows mongod 网络 | 中 | 先 `mongosh`/`mongo` ping；文档写明 |
| `map_cfg_id_` 为 uint64 → int32 | 低 | 范围检查 |
| 关服丢最后几笔写 | 中 | `WaitForIdle`；后续可加离图强制 flush |
| CODEBUDDY：勿改仓库 `third_party/` | — | 驱动已放仓库根 `third_party/mongo-*`；业务逻辑仍只改 `src/mongo` |
| CMake 目标名 `mongo` 过短 | 低 | 仅本工程子目录目标；与上游 `mongoc_*` 不冲突 |
| VS rsync `--exclude=build` 导致找不到 `BuildVersion.cmake` | 高 | **禁止**用未锚定的 `build` 做同步排除；产物目录用 `out/`（见 `CMakePresets.json`）。驱动模块在 `mongo-c-driver/build/cmake/` |

线程模型：游戏服单线程 EventLoop；Mongo worker 多线程。跨线程边界只有无锁快照值与 dispatcher 队列。

### 10.1 VS 远程 / WSL 构建注意

- Preset `binaryDir`：`${sourceDir}/out/build/<preset>`，rsync 排除 `out` 即可，**不要**排除全局名 `build`。
- **不要**把 `src/zlib-1.3.1` 放进 exclusionList：会误伤 `third_party/mongo-c-driver/src/zlib-1.3.1`（BUNDLED zlib）。
- 若远程报 `include could not find BuildVersion`：检查 `/root/.vs/game/third_party/mongo-c-driver/build/cmake/` 是否存在；不存在就是被 rsync 误排除了，改 exclusionList 后重新同步。

---

## 11. 拟修改 / 新增文件清单（确认后再动）

### 结构调整

- `src/mongoStandalone/MongoStandalone/**` → `src/mongo/**`
- 头文件目录与 `#include` / `namespace`：`mongo_standalone` → `mongo`
- CMake 目标：`mongo_player_storage` → `mongo`

### 构建

- `CMakeLists.txt`（根）：`add_subdirectory`、link、宏、`INC_DIRS`
- `src/mongo/CMakeLists.txt`：目标名与 option 对齐

### 游戏服

- `game_server.h` / `game_server.cpp` — 持有 / 启停 storage
- `handlers/move_handler.cpp` — 移动后异步落地
- **新增** `systems/player_persist_system.h` / `.cpp` — 快照组装 + 节流

### 测试

- **新增** `tests/player_mongo_persist_test.cc`
- `CMakeLists.txt` — 加入对应测试目标源文件

### 文档

- 本文件（实现后改「状态：已落地」）
- `doc/server/README.md`、`architecture.md`、`flows-move.md`、`file-matrix.md`、`testing.md`、必要时 `faq.md`

---

## 12. 确认项（请回复后开始写代码）

已确认：

1. **目录 / namespace / CMake 目标** 统一为 `mongo`（`src/mongo/`）。

待确认：

2. **移动节流**默认 `500ms` 是否合适？是否需要「离图强制写一次」纳入 MVP？
3. **默认启用策略**：仅 `GAME_MONGO_ENABLE=1` 才初始化，还是检测到 `MONGO_URI` 即启用？
4. 测试是挂进现有 `svc_game_3d_test`（`GAME_MONGO_TEST=1` 门控），还是独立 `svc_game_3d_mongo_test`？
5. 本阶段是否 **只做构建接入 + 移动落地 + 测试**，加载存档留后续？

确认后按第 9 节 A→E 执行。

---

## 13. 实现落地说明（已实现 C/D，E 待补）

### 13.1 已实现内容

步骤 A/B 此前已完成（目录 `src/mongo/`、namespace `mongo`、CMake 目标 `mongo`、根 `CMakeLists.txt` 链接 + `GAME_HAS_MONGO=1`）。本次补齐 C/D：

| 步 | 文件 | 说明 |
|----|------|------|
| C | `src/gameserver/systems/player_persist_system.h` / `.cpp`（新增） | 快照组装（读 Role/Map/Transform 组件，值语义）+ 按 `role_id` 节流 + 进程内单调 `sequence` + 委托 `PostSave` + `FlushOnShutdown`（`RequestStop`+`WaitForIdle(5s)`+`Stop`） |
| C | `src/gameserver/game_server.h` | `#ifdef GAME_HAS_MONGO` 加 `player_storage_` / `player_persist_` 成员、`GetPlayerPersist()`、`PersistPlayerAfterMove()` |
| C | `src/gameserver/game_server.cpp` | `Start` 按 `GAME_MONGO_ENABLE`/`MONGO_URI` 初始化（try/catch，失败仅 WARN 不阻断启动）；`DoGracefulStop` 在 `LeaveMap` 后、`server_.Stop` 前调 `FlushOnShutdown`；实现 `PersistPlayerAfterMove` |
| D | `src/gameserver/handlers/move_handler.cpp` | `MoveEntity` → `OnMove` 后、`SendMoveRes` 前 `#ifdef GAME_HAS_MONGO` 调 `PersistPlayerAfterMove` |
| 构建 | `CMakeLists.txt` | `svc_game_3d_server` 源列表加 `player_persist_system.cpp` |

### 13.2 编译接入说明

mongo 代码总是参与编译（无条件编译开关，已移除 `GAME_HAS_MONGO`）。所有引用 `game_server.cpp` / `move_handler.cpp` 的目标（`svc_game_3d_server` 及 3 个 protocol test）均链接 `mongo` 并编译 `player_persist_system.cpp`；不引用 `game_server.cpp` 的 test 目标（`svc_game_3d_test` / `svc_game_3d_proto_test`）不受影响。运行时是否真正初始化存储由环境变量 `GAME_MONGO_ENABLE` / `MONGO_URI` 控制（见 13.3）。完整改动记录见 `doc/server/mongo_changes.md`。

### 13.3 启用与节流策略（文档第 12 节待确认项的决策）

- **启用策略**：`GAME_MONGO_ENABLE=1` 显式开；`=0` 显式关；未设时若 `MONGO_URI` 已配置则开，否则默认关。
- **节流**：默认 `500ms`，环境变量 `GAME_MONGO_MOVE_PERSIST_MS` 覆盖，`0` 关闭移动落地。
- **离图强制 flush**：MVP **不做**，仅 `shutdown` 时 `FlushOnShutdown` 排空已入队任务。

### 13.4 线程模型与生命周期

- `PlayerPersistSystem` 的 `last_post_ms_` / `sequences_` 仅在 GameServer 主线程（EventLoop）访问——`MoveHandler::Handle` 与 `DoGracefulStop` 均在 loop 线程执行，**无需加锁**。
- worker 线程只接触值拷贝 `PlayerSnapshot`（`PostSave` 内 `std::move`），**禁止也不持有 `Entity*`**。
- `storage_` 由 `GameServer` 拥有；`FlushOnShutdown` 在 loop 线程排空后，`~PlayerMongoStorage` 析构幂等 `Stop`。
- `MongoClient` 构造只解析 URI、建 `mongocxx::pool`（延迟连接），不立即连 mongod；真正写库在 worker，连不上走 `ErrorHandler` 打 WARN，不回滚内存位置。

### 13.5 字段映射

| `PlayerSnapshot` | 来源 | 备注 |
|------------------|------|------|
| `id` | `RoleComponent::role_id_`，无则 `entity->GetId()` | uint64→int64 |
| `name` / `level` | `RoleComponent` | |
| `position.mapId` | `MapComponent::map_cfg_id_` | uint64→int32，超 `INT32_MAX` 钳制 + WARN |
| `position.x/y/z` | `TransformComponent::pos_` | float→double 提升无损 |
| `sequence` | 进程内 `role_id` 单调递增 | 乱序覆盖可观测 |
| attributes / skills / autoPick | MVP 默认值 | 登录落地阶段补 |

### 13.6 可改进点（后续）

1. **离图强制 flush**：玩家 `LeaveMap` / 被踢时强制 `PostSave` 一次最新坐标，避免节流窗口内最后位置丢失。当前 MVP 仅 shutdown 排空。
2. **节流表清理**：`last_post_ms_` / `sequences_` 按 `role_id` 累积，长时间运行内存增长。建议离图时清理，或加 LRU 上限。
3. **sequence 持久化**：当前进程内，重启归零；重启后旧文档 `last_sequence` 可能大于新写，乱序覆盖判定失效。可接受，后续若需要可在加载存档时恢复。
4. **attributes / skills / autoPick 填充**：MVP 填默认，登录落地阶段补齐。
5. **登录/进图加载存档**：当前为非目标；后续从 `players` 集合 `FindOne` 恢复位置/属性。
6. **可观测性**：`Metrics()`（posted/completed/failed/rejected/queued/active）已具备，建议接入监控导出。
7. **shutdown 阻塞**：`FlushOnShutdown` 在 loop 线程阻塞最多 5s，期间不处理网络事件；shutdown 阶段可接受。若需更平滑可改为定时轮询 `pending`。
8. **测试**（第 8.2 节用例）待补：`tests/player_mongo_persist_test.cc`（`GAME_MONGO_TEST=1` 门控），用例：BuildSnapshotFromEntity / PostSaveAfterMove_RoundTrip / ThrottleDropsBurst / DisabledWhenEnvOff。

### 13.7 编译与验证

```bash
# WSL 内（默认 GAME_HAS_MONGO 已开启）
cmake . && make -j$(nproc) svc_game_3d_server

# 运行（需先启 mongod，见第 7 节）
GAME_MONGO_ENABLE=1 ./bin/svc_game_3d_server
# 或依赖 MONGO_URI 已设：MONGO_URI=mongodb://127.0.0.1:27017/?directConnection=true ./bin/svc_game_3d_server

# 节流调整 / 关闭
GAME_MONGO_MOVE_PERSIST_MS=200 ./bin/svc_game_3d_server   # 200ms
GAME_MONGO_MOVE_PERSIST_MS=0   ./bin/svc_game_3d_server   # 关闭移动落地

# 手工验证（mongosh）
db.players.find({_id: <role_id>})
```

