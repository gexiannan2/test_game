<!-- 拆自 CODE_FLOWS；权威目录 doc/server/ -->

## 20. 续问索引（给 AI / 后人）

下次提问可直接引用章节号或关键词：

| 想问什么 | 看章节 |
|----------|--------|
| 启动顺序 | §3 |
| 某个协议包 | §4 |
| 进图谁推谁 | §14 |
| 移动跨格 | §15 |
| 断线重连 | §16 |
| AOI 内部结构 | §8 |
| 组件何时挂 | §11 |
| 常量 | §17 |
| 踩坑 | §18 |
| 测什么 | §19 |
| 协议号 / err_code | §21 |
| 坐标换算 | §22 |
| 文件找谁改 | §23 |
| 帧格式 | §24 |
| 登录顶号细节 | §25 |
| Enter/Move/Leave 逐步 | §26 |
| 排障 | §27 |
| 历史 bug | §28 |
| 出生点配置 | §29 |
| 联调清单 | §30 |

**相关源码入口速查**：

- 编排：`game_server.cpp`  
- 进图：`handlers/game_handler.cpp` → `world_system.cpp::EnterMap`  
- 移动：`handlers/move_handler.cpp` → `MoveEntity`  
- 视野推包：`aoi_view_bridge.cpp` + `aoi_sector.cpp::AddReceiver/SwitchMonitor`  
- 重连：`handlers/connection_handler.cpp`  
- 协议号：`src/protocol/pack_flags.h`  
- 坐标：`src/common/vector3d.h`

---
