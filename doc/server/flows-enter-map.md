<!-- 拆自 CODE_FLOWS；权威目录 doc/server/ -->

## 14. 进图 AOI 双向视野详解

**问题**：进图后，周围玩家会不会推给自己？**会。**

| 方向 | 触发 | 机制 |
|------|------|------|
| 别人看到你 | `OnSubjectEnterMap` → `MonitorEntity` → `NotifyAppearAllReceivers` | 本格已有观察者收到你的 appear |
| **你看到别人** | `AddWatcher` → `AttachWatcher` → `AddReceiver` | `CollectMonitorNodeIds` + `NotifyAppearToReceiver` |
| 你看到自己 | Handler `BuildAppearBody(self,self)` | AOI 故意不推自己 |
| 进场景 | `NotifyEnterMap` | `cli_3d_enter_map` |

`AddReceiver` 注释原文含义：新增观察者后立刻把本格已有物体以「进入视野」推给它。

---
