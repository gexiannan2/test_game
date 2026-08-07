# 记忆：玩家持久化 / Mongo

**关键词**：Mongo、Persist、FlushPlayer、MarkPersistDirty、entity_player_data  
**最后扫描**：2026-08-05

---

## 启用

```bash
export GAME_MONGO_ENABLE=1
# 可选
GAME_MONGO_PERSIST_INTERVAL_SEC=5
GAME_MONGO_PERSIST_BATCH=100
```

默认 **不连 Mongo**。

---

## 脏标记链

```
Transform 变更
  → PlayerEntity::OnTransformChanged
      → MarkPersistDirty + SetPropertyDirty(kMove) [AOI]

定时 TickPersist（默认 5s）
  → 扫描 dirty → SerializeToDB → PostSave
  → 入队前 ClearDirty（防写风暴）

断线/心跳/kick/GracefulStop
  → FlushPlayer(role_id) 立即落库
```

---

## 关键文件

| 文件 | 职责 |
|------|------|
| `mongo/include/PlayerMongoStorage.h` | 异步 save/load/delete |
| `ecs/systems/player_persist_system.*` | 批量扫描、FlushOnShutdown |
| `ecs/entity/player_entity.cpp` | SerializeToDB、OnTransformChanged |
| `game_server.cpp` | InitMongoIfEnabled、QueryRole、DeleteRoleArchive |

---

## 已知未接线

- **enter_game 未 QueryRole/SyncFromData**：存档可写，重启仍出生点  
- 接入时：进图前异步 Load + `SuppressPersistDirty`

---

## 硬约束

1. PostSave **入队前 ClearDirty**，失败再 SetDirty  
2. 断线 / 超时 / kick / shutdown 均须 **FlushPlayer**  
3. Mongo Post 失败也须回调 completion（已修）

---

## 相关测试

- `mongo/tests/MongoCrudTest.cpp`  
- `ProductionFullstackTest`（若启用 mongo）
