# 记忆：移动 / 跨格 / AOI 脏同步

**关键词**：move、MoveEntity、SetMoveState、SwitchMonitor、MoveWatcher、SetPropertyDirty  
**最后扫描**：2026-08-05 | 测试 **127/127** 通过

---

## 调用链（客户端移动）

```
cli_3d_move_req
  → MoveHandler
      → 校验 IsInMap + kInGame + isfinite(pos/rot/vel)
      → ClampToMapBounds（Jolt AABB）
      → SetMoveState(rot, move_rot, vel)     // ① 先写朝向/速度
      → WorldSystem::MoveEntity(entity, pos) // ② 再改位置
          → SetPosition → OnTransformChanged → SetPropertyDirty(kMove)
          → [跨 Map 格] map_->OnEntityChangePos
          → aoi_.OnEntityChangePos（跨 AOI 格 → SwitchMonitor）
          → aoi_.MoveWatcher（GetGridCenter 变时重建观察者邻域）
      → cli_3d_move_res（仅请求方）

30Hz Tick
  → AoiSystem::FlushDirty → cli_3d_aoi_update_ntf
```

**顺序铁律**：禁止先 MoveEntity 再 SetMoveState（Jolt 读旧 rot）。

---

## 同格 vs 跨格

| 情况 | Map 足迹 | AOI appear/disappear | 脏 update |
|------|----------|----------------------|-----------|
| 同 Map 格、同 AOI 格 | 不变 | 无 | **须** SetPropertyDirty(kMove) |
| 跨 Map 格 / 跨 AOI 格 | OnEntityChangePos | SwitchMonitor + MoveWatcher 差集 | 跨格移动自动标脏 |

同格脏同步：`PlayerEntity::OnTransformChanged` → `SetPropertyDirty(kMove)`。

---

## AOI 常量（`aoi_def.h` / `vector3d.h`）

| 常量 | 值 | 说明 |
|------|-----|------|
| `kGridSize` | 4m | Map 逻辑格；`GetGridCenter` 基准 |
| `kAoiCellWorldSize` | 10m | AOI 视野格 |
| `kNeighborhoodRadius` | 1 | 观察者邻域 ±1 格（3×3×3） |
| `kDetailLevelCount` | 1（远景层可编译关闭） | 观察者只挂一层 |

**观察中心**：`AddWatcher` / `MoveWatcher` 用 `GetGridCenter()`（Map 格中心），不是裸脚底坐标。  
**Subject 家格**：`GetPosition()` 算 AOI 格索引。

---

## 关键文件

| 文件 | 职责 |
|------|------|
| `handlers/move_handler.cpp` | 协议、钳制、顺序 |
| `ecs/systems/world_system.cpp` | MoveEntity 总线 |
| `ecs/systems/aoi_system.cpp` | MoveWatcher、FlushDirty |
| `ecs/systems/aoi_sector.cpp` | SwitchMonitor、EntityMonitor |
| `map_bounds_util.h` | ClampToMapBounds |
| `ecs/systems/move_system.cpp` | NPC 寻路 Tick（非玩家 cli move） |

---

## 硬约束

1. MoveEntity 用 **钳制后** 坐标改格  
2. Y 轴与 XZ 一样 **向内** 钳制  
3. AOI **不推自身** update/disappear（appear 仅进图 `NotifySelfAppear` 一次）  
4. 多层 FlushDirty：**所有 sector 推完再 ClearPropertyTypes**  
5. MoveWatcher 失败 → Remove+Add 自愈；Add 失败回退 old_center  

---

## 已知风险（未修）

| 项 | 说明 |
|----|------|
| MoveWatcher 自愈 | `RemoveWatcher(false)` + `AddWatcher` 可能向邻居 **重复推 appear**（notify=false 不先发 disappear） |
| `HasWatcher` | 仅查观察中心格，邻域其他格不查；半径>0 时中心格应始终有 receiver |

---

## 相关测试

```bash
GAME_TEST_FILTER=AoiAppearDisappear ./bin/svc_game_3d_test
GAME_TEST_FILTER=AoiBroadcast ./bin/svc_game_3d_test
GAME_TEST_FILTER=AoiMultiLevel ./bin/svc_game_3d_test
GAME_TEST_FILTER=Regression ./bin/svc_game_3d_test
```

- `AoiAppearDisappearTest` — 互见 / 离开 / 重进  
- `AoiBroadcastTest` / `AoiParityTest` — Oracle 对拍  
- `AoiMultiLevelTest` — 近远景（`kDetailLevelCount>1` 时）  
- `RegressionTest`、`EdgeCaseTest`
