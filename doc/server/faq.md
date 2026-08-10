<!-- 拆自 CODE_FLOWS；权威目录 doc/server/ -->

## 27. 排障 FAQ

| 现象 | 可能原因 | 排查 |
|------|----------|------|
| 进图看不到周围人 | AddWatcher 失败 / 不在同 AOI 格 / Bridge SendFn 找不到 conn | 查 `NeedsAoiWatcher`、距离是否 >10m、`FindEntity` |
| 周围看不到我 | OnSubjectEnterMap 未走 / LeaveMap 过早 | 查 `IsInMap`、日志 EnterMap |
| 有自己没别人 | `AddWatcher` / `NotifyAppearToReceiver` 异常 | 断点 `AddReceiver`、`MonitorEntity` |
| 移动后别人位置不更新 | 同格不广播（设计如此）；或 MoveEntity/OnMove 顺序反了 | 确认先 MoveEntity；跨格才 disappear/appear |
| 重连后不能动 | EnterMap 被 IsInMap 短路；或 stale disconnect 误 Leave | 查重连先 Leave 再 Enter；conn 归属 |
| 重登后被踢出图 | 旧 disconnect 误清理 | 须 SetContext{} + OnConnection stale 判断 |
| 旧设备仍能 reconnect | 未 UnregisterBySessionId | 查重登 session 轮换 |
| appear 缺朝向 | SerializeDirty 漏 move_rot | 对拍 aoi_proto_test |
| 心跳误踢 | last_heartbeat 未刷新 | 查 HeartBeat Handler |
| 粘包第二帧丢逻辑 | OnMessage 未每帧刷新 Context | 已修：每帧 any_cast |

---
