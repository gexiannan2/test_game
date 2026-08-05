<!-- 拆自 CODE_FLOWS；权威目录 doc/server/ -->

## 8. AoiSystem / Sector / Monitor 视野

### 8.1 常量（`aoi_def.h` / `vector3d.h`）

| 常量 | 值 | 含义 |
|------|-----|------|
| `kAoiCellWorldSize` | 10 | AOI 格边长（米） |
| `kDetailLevelCount` | 1 | 当前仅近景一层 |
| `kAoiRadius` / `kNeighborhoodRadius` | 0 | 观察者只看**中心一格**（10×10×10） |
| `kAoiFarCellWorldSize` | 500 | 远景 LOD 预留，未启用 |

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

---

---

## 9. AoiViewBridge 网络桥接

**文件**：`aoi_view_bridge.h/.cpp`  
**Install 绑定**：

| 回调 | 组包 |
|------|------|
| enter | `Entity::SerializeAppear` → 通常 `kCli3dEntityAppearEx` |
| leave | `BuildDisappearBody` → `kCli3dEntityDisappearEx` |
| update | `Entity::SerializeDirty` |
| Map enter_map | `cli_3d_enter_map`（给自己） |

**公开复用**：`BuildAppearBody(observer, observee)` —— Handler 发自身 appear 用。

协议无关：只负责找 conn 与 `SendFn`；字段由 Entity 子类决定。

---
