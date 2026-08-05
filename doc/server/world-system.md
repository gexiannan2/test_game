<!-- 拆自 CODE_FLOWS；权威目录 doc/server/ -->

## 6. WorldSystem 场景总线

**文件**：`systems/world_system.h/.cpp`

### 6.1 公开 API

`Create` / `Init` / `Tick` / `SetEntityFactory` / `Spawn` / `SpawnOnMap` /  
`EnterMap` / `LeaveMap` / `MoveEntity` / `UpdateEntity` /  
`GetVisibleEntities` / `IsWatcher` / `AllocateEntityId` /  
`RegisterEntity` / `UnregisterEntity` / `FindEntity` / `GetEntityCount` /  
`Map()` / `Aoi()` / `Move()`

### 6.2 EnterMap（幂等：已在图则 return）

```
RegisterEntity
→ map_->OnEntityIntoMap
→ SetInMap(true) + SetWorld(this)
→ aoi_.OnSubjectEnterMap          // 被观察者
→ [NeedsAoiWatcher] aoi_.AddWatcher(GetGridCenter())  // 观察者
→ map_->NotifyEnterMap            // → Bridge → cli_3d_enter_map
```

### 6.3 LeaveMap（幂等：未在图则 return）

```
aoi_.OnSubjectLeaveMap            // 周围 disappear
→ RemoveWatcher(..., notify=false)
→ SetInMap(false) + SetWorld(nullptr)
→ ClearPropertyTypes()            // 清 CancelMove 残留脏位
→ map_->OnEntityLeaveMap
→ UnregisterEntity
→ map_->NotifyLeaveMap
```

### 6.4 MoveEntity

```
若 !IsInMap：仅 SetPosition，return
old_center = GetGridCenter()      // 必须在 SetPosition 前
→ SetPosition(new)
→ map_->OnEntityChangePos
→ aoi_.OnSubjectMoved
→ [玩家观察者] MoveWatcher(old_center, new_center)
→ map_->NotifyMove
```

### 6.5 Tick

`MoveSystem::Tick(1/60)` → `AoiSystem::FlushDirty`

### 6.6 UpdateEntity

仅 `IsInMap` 时 `MarkPropertyDirty`（防离图幽灵更新）

---
