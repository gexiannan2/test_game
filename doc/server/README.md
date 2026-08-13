# svc_game_3d_server 文档索引

> **权威目录**：`doc/server/`  
> **源码**：`src/gameserver/`  
> **Cursor 规则**：`.cursor/rules/server-code-flows.mdc`  
> **更新约定**：改进图 / 移动 / AOI / 登录 / 断线时，同步改对应模块 md。

## 模块文档

### 架构与编排

| 文档 | 内容 |
|------|------|
| [architecture.md](architecture.md) | 分层、双索引、目录地图 |
| [async-jolt-one-frame-pipeline.md](async-jolt-one-frame-pipeline.md) | 异步 Jolt：晚 1 帧双缓冲管线与 Apply 含义 |
| [gameserver.md](gameserver.md) | Start / 连接 / 收包 / 心跳 / 发包 |
| [file-matrix.md](file-matrix.md) | 源码文件职责矩阵 |
| [constants.md](constants.md) | 常量与 ID 空间 |
| [constraints.md](constraints.md) | 设计约束与已知坑 |

### 协议与网络

| 文档 | 内容 |
|------|------|
| [handlers.md](handlers.md) | 各 Handler 逐包行为 |
| [protocol.md](protocol.md) | 协议号、err_code、帧编解码 |
| [coordinates.md](coordinates.md) | Map 4m / AOI 10m 坐标换算 |

### 业务 System / 场景

| 文档 | 内容 |
|------|------|
| [system-facade.md](system-facade.md) | System::*、PES、MapConfig、PlayerConfig |
| [world-system.md](world-system.md) | WorldSystem Enter/Leave/Move/Tick |
| [map-system.md](map-system.md) | MapSystem + 地图配置表 |
| [aoi-system.md](aoi-system.md) | AOI / Sector / Monitor / ViewBridge |
| [move-system.md](move-system.md) | MoveSystem / MoveComponent（NPC） |
| [ecs.md](ecs.md) | Entity 状态机与 Component |
| [mongo.md](mongo.md) | 玩家 Mongo 异步落地（`src/mongo`） |

### 业务流程

| 文档 | 内容 |
|------|------|
| [flows-lifecycle.md](flows-lifecycle.md) | 从 0 到 1 全生命周期 |
| [flows-login.md](flows-login.md) | 登录 / 顶号 / 选角时序 |
| [flows-enter-map.md](flows-enter-map.md) | 进图双向视野 |
| [flows-move.md](flows-move.md) | 移动与跨格 AOI |
| [flows-disconnect.md](flows-disconnect.md) | 离图 / 断线 / 心跳 / 重连 |
| [flows-call-tables.md](flows-call-tables.md) | EnterMap / MoveEntity / LeaveMap 逐步表 |

### 联调与质量

| 文档 | 内容 |
|------|------|
| [testing.md](testing.md) | 测试体系 |
| [faq.md](faq.md) | 排障 FAQ |
| [changelog-bugs.md](changelog-bugs.md) | 历史缺陷与修复 |
| [checklist.md](checklist.md) | 客户端联调清单 |
| [index-topics.md](index-topics.md) | 续问关键词 → 文档映射 |

## 快速入口

- **AI 记忆（优先）** → [../memory/INDEX.md](../memory/INDEX.md)  
- 进图谁推谁？ → [flows-enter-map.md](flows-enter-map.md)  
- 移动顺序？ → [flows-move.md](flows-move.md) + [constraints.md](constraints.md)  
- 重连顶号？ → [flows-disconnect.md](flows-disconnect.md) + [flows-login.md](flows-login.md)  
- 协议号？ → [protocol.md](protocol.md)  
- 改哪个文件？ → [file-matrix.md](file-matrix.md)
