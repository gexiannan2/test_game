<!-- 拆自 CODE_FLOWS；权威目录 doc/server/ -->

## 15. 移动与跨格 AOI

```
cli_3d_move_req
  → MoveEntity(pos)     // 先：旧中心 → 设位置 → Map → OnSubjectMoved → MoveWatcher
  → OnMove(rot/vel)     // 后：写 Transform
  → move_res 仅自己
```

- **同 AOI 格**：无 appear/disappear；别人看不到你同格挪动（当前设计不标脏广播）。  
- **跨 AOI 格**：`SwitchMonitor` + 观察者 `MoveWatcher` 差集。  
- **顺序铁律**：禁止先 `OnMove` 再 `MoveEntity`（会令 old==new，漏跨格）。

---
