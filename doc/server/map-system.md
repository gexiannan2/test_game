<!-- 拆自 CODE_FLOWS；权威目录 doc/server/ -->

## 7. MapSystem 逻辑地图

**文件**：`map_system.h/.cpp`，`map_grid.h/.cpp`  
**格边长**：`kGridSize = 4`（`common/vector3d.h`）  
**形态**：无界稀疏 `unordered_map`；`IsInMap(pos)` 恒 true

### 7.1 职责

- 实体占地足迹：进/离/换格
- 格中心换算：`MapIndexToCenterPos`（供 AOI 观察中心）
- 回调：`NotifyEnterMap` / `NotifyLeaveMap` / `NotifyMove` / `NotifyCrossGrid`
- 辅助：`LineInterGrid`、`CollectEntitiesInGridBox`（两阶段收集，防回调重入）

### 7.2 与 AOI

Map 管「谁在哪个 4m 格」；AOI 管「谁看见谁」。`AoiCell::SyncSubjectsFromMap` 可对齐主体集合。

---

---

## 29. 地图配置表（当前 LoadDefaults）

| cfg_id | 名称 | res_id | born_pos (x,y,z) | born_range |
|--------|------|--------|------------------|------------|
| **1001** | 新手村 | 1001 | (333, 18, 415.45) | 3 |
| 1201 | 野外森林 | 1201 | (333.80, 4.36, 303.43) | 3 |

`GetFirstMap()` → **1001**（`ordered_` 插入顺序首张）。  
进图默认 `map_ins_id_ = 1`。

---
