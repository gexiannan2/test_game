<!-- 拆自 CODE_FLOWS；权威目录 doc/server/ -->

## 1. 架构总览

### 1.1 分层（自下而上）

```
Entity + Component     数据与序列化、会话状态机
        ↑
System（静态）         账号/角色/连接/Transform 写入（无网络、无场景）
        ↑
WorldSystem            场景总线：Map + AOI + Move + entity_index_
        ↑
Handler                协议解包 → 调 System/World → 回包
        ↑
GameServer             TCP、Handler 注册、心跳、Tick、ID 生成
        ↑
main.cc                进程入口
```

### 1.2 两条实体索引（切勿混淆）

| 索引 | 类 | Key | 生命周期 |
|------|-----|-----|----------|
| 跨连接缓存 | `PlayerEntitySystem` | uid / session_id / role_id | 断线**保留**，供重登/重连 |
| 在图注册表 | `WorldSystem::entity_index_` | entity_id | 仅 `EnterMap`～`LeaveMap` |

### 1.3 生产驱动模型

- **玩家移动**：客户端 `cli_3d_move_req` 驱动，不经 `MoveSystem::RequestMoveTo`。
- **世界 Tick**：`GameServer` 有约 60Hz `world_->Tick()`（推进 NPC `MoveSystem` + `FlushDirty`）；玩家摇杆路径不依赖 Tick 做跨格。
- **AOI 事件**：进图/移动跨格/离图时**同步**产生 appear/disappear，经 `AoiViewBridge` 发包。

---

---

## 2. 目录与模块地图

```
src/server/
├── main.cc / game_server.h/.cpp
├── mongo/             # 玩家 Mongo 异步落地（INTERFACE 源码 + 驱动）
├── handlers/          # 协议入口
│   ├── handler_base.h
│   ├── connection_handler.*   # 握手/心跳/重连/踢人
│   ├── account_handler.*      # 登录
│   ├── role_handler.*         # 角色 CRUD/选角/随机名
│   ├── game_handler.*         # enter_game 进图
│   └── move_handler.*         # 3D 移动
├── systems/
│   ├── world_system.*         # 场景总线
│   ├── map_system.* / map_grid.*
│   ├── aoi_def.h / aoi_system.* / aoi_sector.*
│   ├── aoi_view_bridge.*
│   ├── move_system.*
│   ├── system.h               # MapConfig/PlayerConfig/PES/System 门面
│   ├── *_system.cpp           # connection/account/role/game/map_config/player_entity
│   └── ...
├── ecs/
│   ├── entity/entity.* / player_entity.*
│   ├── component_base/
│   └── components/            # connection/account/role/transform/map/move
├── tests/                     # 见第 19 节
└── CODE_FLOWS.md              # 本文
```

---
