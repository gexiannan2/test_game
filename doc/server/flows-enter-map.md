<!-- 拆自 CODE_FLOWS；权威目录 doc/server/ -->

## 14. 进图 AOI 双向视野详解

**问题**：进图后，周围玩家会不会推给自己？**会。**

| 方向 | 触发 | 机制 |
|------|------|------|
| 别人看到你 | `OnEntityIntoMap` → `MonitorEntity` → `NotifyAppearAllReceivers` | 本格已有观察者收到你的 appear |
| **你看到别人** | `AddWatcher` → `AttachWatcher` → `AddReceiver` | `CollectMonitorNodeIds` + `NotifyAppearToReceiver`（跳过自身） |
| 你看到自己 | `AoiSystem::NotifySelfAppear`（`EnterMap` 末尾） | 桥接层克隆包设 `is_self=true`；**仅进图一次** |
| 进场景 | `NotifyEnterMap` → `AoiViewBridge` | `cli_3d_enter_map_ntf` |

`AddReceiver` 注释原文含义：新增观察者后立刻把本格已有物体以「进入视野」推给它。

### A 先进、B 后进（同邻域）

1. A 进图 → A 收 1 次 self appear  
2. B 进图 → A 收 B appear（**不再**收 A）；B 收 self + A appear  
3. B 移出视野 → 双方互相 disappear  
4. B 移回 → 双方互相 appear  

联调：`src/client/aoi_enter_leave_test.cc`。

---
