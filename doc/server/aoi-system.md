<!-- 拆自 CODE_FLOWS；权威目录 doc/server/ -->

## 8. AoiSystem / Sector / Monitor 视野

### 8.1 常量（`aoi_def.h` / `vector3d.h`）

| 常量 | 值 | 含义 |
|------|-----|------|
| `kAoiCellWorldSize` | 10 | 近景 AOI 格边长（米） |
| `kAoiFarCellWorldSize` | 500 | 远景（大地图）AOI 格边长 |
| `kDetailLevelCount` | 2 | 近景 + 远景两层；观察者只挂一层 |
| `kAoiDetailNear` / `kAoiDetailFar` | 0 / 1 | `AddWatcher(..., detail_level)` |
| `kAoiRadius` / `kNeighborhoodRadius` | 1 | 观察者邻域 ±1 格（近景≈30m，远景≈1500m） |

### 8.2 层次

```
AoiSystem
  └── AoiSector[]（按 detail_level）
        └── AoiCell（稀疏哈希）
              └── EntityMonitor
                    ├── receivers_   // 观察者
                    └── monitor_nodes_ // 被观察者
```

### 8.3 被观察者 API

- `OnSubjectEnterMap` → 进格 `MonitorEntity` → `NotifyAppearAllReceivers`（别人看到你）
- `OnSubjectLeaveMap` → `UnmonitorEntity(notify)` + 全分区静默清理
- `OnSubjectMoved` → 同格无事；跨格 → `SwitchMonitor`

### 8.4 观察者 API

- `AddWatcher` → `AttachWatcher` → 邻域各格 `AddViewer` → `AddReceiver`  
  → **立刻把本格已有主体 appear 推给新观察者**（你看到别人）
- `NotifySelfAppear` → 进图补发自身 appear（`is_self=true`），与 `NotifyAppearToReceiver` 跳过自身配合
- `MoveWatcher` → 新旧邻域差集：新格 appear、旧格 disappear
- `RemoveWatcher(notify=false)` 离图时用，避免给自己刷 disappear

### 8.5 SwitchMonitor（跨格核心）

观察者分三组：

- 仅新格有 → appear  
- 仅旧格有 → disappear  
- 两边都有 → 若 subject 脏：即时 `NotifyUpdated` 后 `ClearPropertyTypes`（**不再入脏队列**，避免双重 kUpdate）
- **不向自身推 appear / update / disappear**（`watcher_id == subject_id` 过滤）

自过滤：`NotifyAppearToReceiver` 排除 `watcher_id == subject_id`。

### 8.6 脏同步

`MarkPropertyDirty` → 入格脏队列 → `FlushDirty`/`sync_immediately` → `PushEntityUpdate` → Bridge `SerializeDirty`  
守卫：`subject->IsInMap()`；`PushEntityUpdate` 要求仍在 `monitor_nodes_`。

**多层优化（`kDetailLevelCount > 1`）：**

- `MarkPropertyDirty` **不** `EnsureCell`；仅对「已监控该 subject 且格内有 receiver」的 sector 入脏队列（避免远景空 `dirty_entities_`）。
- **`FlushDirty` 禁止在 sector 内提前 `ClearPropertyTypes`**：近/远景会各自 `PushEntityUpdate`；由 `AoiSystem` 收齐所有层后再统一清脏（否则远景观察者丢 update）。
- `OnSubjectLeaveMap`：家格 `Unmonitor(notify=true)` 成功则跳过全表扫；家格未命中才 `ForEachCellUntil` 清幽灵并 early-exit。
- 观察者切换小/大地图：`RemoveWatcher` + `AddWatcher(..., kAoiDetailFar)`，per-sector receivers 隔离，无需改广播管线。

---

---

## 9. AoiViewBridge 网络桥接

**文件**：`aoi_view_bridge.h/.cpp`  
**Install 绑定**：

| 回调 | 组包 |
|------|------|
| enter | `Entity::SerializeAppear` → `cli_3d_aoi_appears_ntf` |
| leave | `BuildDisappearBody` → `cli_3d_aoi_disappears_ntf` |
| update | `Entity::SerializeDirty` → `cli_3d_aoi_update_ntf` |
| Map enter_map | `cli_3d_enter_map_ntf`（给自己） |

**自身 appear**：由 `WorldSystem::EnterMap` → `NotifySelfAppear` 经广播发出；桥接层对 `watcher_id == subject_id` 克隆并设 `is_self=true`。Handler **不再**手动 `BuildAppearBody(self)`。

`BuildAppearBody` 仍可供测试/工具复用序列化路径。

协议无关：只负责找 conn 与 `SendFn`；字段由 Entity 子类决定。

---
