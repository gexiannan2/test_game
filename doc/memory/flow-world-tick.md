# 记忆：World Tick / 仿真流水线

**关键词**：30Hz、Tick、MoveSystem、JoltSystem、FlushDirty、EventLoop  
**最后扫描**：2026-08-05

---

## 启动顺序（GameServer::Start）

```
LoadConfigs → InitWorldAndAoi → RegisterAllHandlers
→ InitMongoIfEnabled → InitServerNetwork → StartTimers
```

---

## 30Hz World Tick（同 EventLoop 线程）

```
game_server.cpp world_tick_timer_ (kWorldTickIntervalSec ≈ 1/30)
  → WorldSystem::Tick(dt)
      1. MoveSystem::Tick        // NPC 寻路
      2. JoltSystem::SyncAllBodies
      3. JoltServer::Update
      4. JoltSystem::PostUpdate
      5. AoiSystem::FlushDirty   // 脏属性 → update 推包
```

---

## EnterMap / LeaveMap / MoveEntity（场景总线）

| API | 顺序要点 |
|-----|----------|
| `EnterMap` | 足迹 → AOI subject → AddWatcher → NotifyEnterMap |
| `LeaveMap` | AOI leave → CancelMove → SetInMap(false) → ClearPropertyTypes → 足迹移除 |
| `MoveEntity` | SetPosition → [跨格] map → aoi OnChangePos → MoveWatcher |

**WorldSystem 是唯一场景编排入口**；Handler 禁止直接调 MapSystem/AoiSector。

---

## 关键文件

| 文件 | 职责 |
|------|------|
| `game_server.cpp` | 定时器、InitWorldAndAoi |
| `ecs/systems/world_system.*` | 场景总线 |
| `ecs/systems/move_system.*` | NPC 移动 |
| `ecs/systems/jolt_system.*` | 物理同步 |
| `jolt_server.*` | OBJ 碰撞、边界 AABB |
| `ecs/systems/aoi_view_bridge.*` | AOI → 网络 |

---

## 线程模型

- **单 EventLoop**：网络 + 游戏逻辑 + Tick  
- Mongo：**worker 线程** → `RunInLoop` 回主线程回调

---

## 相关测试

- `IntegrationTest` / `ProductionTest`  
- `LongSoakTest`（默认 2s 随机 churn）  
- `StressTest` / `ExtremeTest`
