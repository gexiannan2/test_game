# Session 层（账号 / 连接 / 角色门面）

与 `ecs/systems/`（世界仿真：AOI / Map / Move / Persist）分离。

| 文件 | 职责 |
|------|------|
| `system.h` | `SessionService::` 静态门面声明 |
| `account_system.cpp` | 登录、session 轮换 |
| `connection_system.cpp` | 握手 / 心跳 / 重连 / kickoff |
| `role_system.cpp` | 角色 CRUD / 选角 |
| `session_restore.cpp` | 顶号 / 重连 / role_list 公共重绑定 |
| `random_name_generator.cpp` | 随机名表与生成 |

Handler 应 `#include "session/system.h"`，重绑定走 `session_restore.h`。
