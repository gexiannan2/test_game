<!-- 拆自 CODE_FLOWS；权威目录 doc/server/ -->

## 10. MoveSystem 服务端移动

**文件**：`move_system.h/.cpp` + `ecs/components/move_component.*`

- **玩家**：不走本系统；`MoveHandler` → `MoveEntity`。
- **NPC/行军**：`RequestMoveTo` → `MoveComponent::TickFrame` → 积分位移 → `world->MoveEntity`（同步地图+AOI）。
- `CancelMove`：停步 + 可标 `kStopMove` 脏。
- `WorldSystem::Tick` 驱动 `MoveSystem::Tick`。

---
