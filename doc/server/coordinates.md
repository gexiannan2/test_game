<!-- 拆自 CODE_FLOWS；权威目录 doc/server/ -->

## 22. 坐标系与格换算

定义：`src/common/vector3d.h`

### 22.1 两套格子

| 体系 | 边长 | 用途 | 索引函数 |
|------|------|------|----------|
| **Map 逻辑格** | `kGridSize = 4` | 实体足迹、占地、格中心 | `WorldToMapGridIndex` / `Vector3D::GridX/Y/Z` |
| **AOI 视野格** | `kAoiCellWorldSize = 10` | 谁看见谁 | `WorldToAoiCellIndex` / `AoiCellX/Y/Z` |

关系：一个 AOI 格大约覆盖 `10/4 = 2.5` 个 Map 格边长；精确覆盖用 `AoiCellToMapGridBox`。

### 22.2 负坐标

换算对负世界坐标做了向下取整语义（见 `math_test.cc`），保证跨原点格子连续。

### 22.3 观察中心

玩家 `AddWatcher`/`MoveWatcher` 使用 **`GetGridCenter()`**（Map 格中心世界坐标），不是裸脚底坐标，避免贴格边抖动。

### 22.4 邻域

`kNeighborhoodRadius = kAoiRadius = 1` → 观察者邻域 ±1 AOI 格（3×3×3）。若将来调大半径，须同步修正 `HasWatcher`（当前仅查中心格）与测试 Oracle。

---
