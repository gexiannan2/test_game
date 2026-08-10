# AI 记忆索引（svc_game_3d）

> **用途**：下次提问时 @ 本目录或具体 `flow-*.md`，快速定位代码与坑。  
> **权威源码**：`src/gameserver/`、`src/client/`、`src/protocol/`、`src/common/`  
> **详细文档**：`doc/server/`（本目录为精简记忆层）  
> **最近全量扫描**：2026-08-05

---

## 怎么用

1. 先在本文件查 **关键词 → 记忆文件**  
2. 打开对应 `flow-*.md` 看调用链、约束、测试命令  
3. 改代码后跑 `doc/memory/UPDATE_PROMPT.md` 里的提示词刷新记忆

---

## 核心流程记忆

| 记忆文件 | 关键词 | 入口源码 |
|----------|--------|----------|
| [flow-connection.md](flow-connection.md) | TCP、握手、心跳、粘包 | `game_server.cpp::OnConnection/OnMessage` |
| [flow-login.md](flow-login.md) | 登录、顶号、session | `account_handler.cpp` → `session/account_system.cpp` |
| [flow-role.md](flow-role.md) | 角色列表、选角、创建删除 | `role_handler.cpp` → `session/role_system.cpp` |
| [flow-enter-game.md](flow-enter-game.md) | 进图、出生点、双向视野 | `game_handler.cpp` → `world_system.cpp::EnterMap` |
| [flow-move-aoi.md](flow-move-aoi.md) | 移动、跨格、脏同步 | `move_handler.cpp` → `MoveEntity` → `aoi_system` |
| [flow-disconnect-reconnect.md](flow-disconnect-reconnect.md) | 断线、重连、踢人 | `connection_handler.cpp` + `OnConnection(false)` |
| [flow-world-tick.md](flow-world-tick.md) | 30Hz、Move/Jolt/AOI Flush | `game_server.cpp` 定时器 → `world_system.cpp::Tick` |
| [flow-persist.md](flow-persist.md) | Mongo、落库、FlushPlayer | `player_persist_system` + `mongo/` |

---

## 横切记忆

| 文件 | 内容 |
|------|------|
| [bugs-and-constraints.md](bugs-and-constraints.md) | P0/P1 硬约束 + 历史 bug 速查 |
| [file-locator.md](file-locator.md) | 改 X 功能该看哪个文件 |
| [UPDATE_PROMPT.md](UPDATE_PROMPT.md) | **每日下班跑**：自动刷新全部记忆 |

---

## 测试一键命令（WSL）

```bash
cd /mnt/d/Dev/tmp1/game
make -j8 svc_game_3d_test && ./bin/svc_game_3d_test
# AOI 专项
GAME_TEST_FILTER=AoiAppearDisappear ./bin/svc_game_3d_test
GAME_TEST_FILTER=AoiBroadcast ./bin/svc_game_3d_test
GAME_TEST_FILTER=LoginAoiChurn ./bin/svc_game_3d_test
GAME_TEST_FILTER=Regression ./bin/svc_game_3d_test
# 客户端 E2E（需起服）
./bin/aoi_enter_leave_test <ip> <port>
```

全量：**127/127** 通过（2026-08-05 WSL 实跑，含 `AoiAppearDisappearTest`）。

---

## 架构一图

```
TcpServer → GameServer::OnMessage → Handler → session/System::
                    ↓
              WorldSystem (30Hz Tick)
         MapSystem / AoiSystem / MoveSystem / JoltSystem
                    ↓
              AoiViewBridge → protobuf → SendMsg
```
