<!-- 拆自 CODE_FLOWS；权威目录 doc/server/ -->

## 26. EnterMap / MoveEntity / LeaveMap 逐步调用表

### 26.1 EnterMap

| 步 | 调用 | 效果 |
|----|------|------|
| 0 | `IsInMap` 短路 | 防重复 |
| 1 | `RegisterEntity` | entity_index_ |
| 2 | `map_->OnEntityIntoMap` | 4m 格足迹 |
| 3 | `SetInMap(true)` + `SetWorld(this)` | 标志 + 脏通知通道 |
| 4 | `aoi_.OnSubjectEnterMap` | 别人收到你的 appear |
| 5 | `aoi_.AddWatcher` | 你收到周围 appear |
| 6 | `map_->NotifyEnterMap` | 你收到 enter_map |
| 7 | Handler `BuildAppearBody(self)` | 你收到自己的 appear |

### 26.2 MoveEntity

| 步 | 调用 | 效果 |
|----|------|------|
| 0 | `!IsInMap` | 仅 SetPosition |
| 1 | 记 `old_pos` / `old_center` | **SetPosition 前** |
| 2 | `SetPosition` | Transform |
| 3 | `map_->OnEntityChangePos` | 足迹搬迁 |
| 4 | `aoi_.OnSubjectMoved` | 跨 AOI → SwitchMonitor |
| 5 | `MoveWatcher` | 观察者邻域差集 |
| 6 | `map_->NotifyMove` | 业务回调（可空） |

Handler 另：`OnMove` 写 rot/vel；`move_res` 仅自己。

### 26.3 LeaveMap

| 步 | 调用 | 效果 |
|----|------|------|
| 0 | `!IsInMap` 短路 | 幂等 |
| 1 | `OnSubjectLeaveMap` | 周围 disappear |
| 2 | `RemoveWatcher(false)` | 卸观察者，不刷自己 |
| 3 | `SetInMap(false)` + `SetWorld(nullptr)` | 断脏通道 |
| 4 | `OnEntityLeaveMap` | 清足迹 |
| 5 | `UnregisterEntity` | 出 entity_index_ |
| 6 | `NotifyLeaveMap` | 业务回调 |

---
