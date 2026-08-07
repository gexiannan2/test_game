# 3D 动态阻挡同步方案

## 1. 协议设计

### 1.1 Proto 文件

协议定义在 `release/e996_lua_game_frame/Protos/client_3d.proto`（与 2D 阻挡 `<br>cli_addblock` / `cli_delblock` 同风格）。

### 1.2 消息一览

| 消息名 | 方向 | 用途 |
|--------|------|------|
| `cli_3d_blockinfo` | — | 单个 3D 阻挡信息 |
| `cli_3d_addblock` | S→C | 添加 3D 阻挡（map→批量） |
| `cli_3d_delblock` | S→C | 删除 3D 阻挡（批量） |

> 与 2D 协议同名但独立编译（字段类型不同：3D 是 float，2D 是 int32）。

### 1.3 字段对照

| 字段 | 2D `cli_blockinfo` | 3D `cli_3d_blockinfo` |
|------|-----|------|
| `BlockType` | int32 | int32（1=圆柱, 2=盒子） |
| `CenterX` | int32（格子坐标） | **float**（世界坐标 x） |
| `CenterY` | int32 | **float**（高度 / min y） |
| `CenterZ` | — | **float**（世界坐标 z） |
| `Range` | int32 | — |
| `Radius` | — | **float**（圆柱半径） |
| `Height` | — | **float**（圆柱高度 / 盒子 max_y） |
| `SizeX` | — | **float**（盒子 max_x） |
| `SizeZ` | — | **float**（盒子 max_z） |

### 1.4 语义

```proto
message cli_3d_blockinfo
{
  int32 BlockType = 1;   // 1=圆柱, 2=盒子
  float CenterX  = 2;    // 圆心 x / 盒子 min x
  float CenterY  = 3;    // 圆心 y(高度) / 盒子 min y
  float CenterZ  = 4;    // 圆心 z / 盒子 min z
  float Radius   = 5;    // 圆柱半径（type=1）
  float Height   = 6;    // 圆柱高度 / 盒子 max_y（type=2）
  float SizeX    = 7;    // 盒子 max_x（type=2）
  float SizeZ    = 8;    // 盒子 max_z（type=2）
}

message cli_3d_addblock
{
  int32 mapinsid = 1;
  map<string, cli_3d_blockinfo> blocks = 2;   // key = tostring(ref)
}

message cli_3d_delblock
{
  int32 mapinsid = 1;
  repeated string blockid = 2;               // 要删除的 tostring(ref) 列表
}
```

客户端收到后：
- **不需要执行 navmesh 操作**，只渲染半透明提示体
- 圆柱 → AABB 近似渲染
- 盒子 → 直接渲染

---

## 2. 消息处理流程

### 2.1 添加阻挡

```
Lua 业务层：ObsMgr.Add(map_cfg_id, map_ins_id, cx, cy, cz, r, h)
  │
  ├── E996.AddObstacle(map_id, cx, cy, cz, r, h, callback)
  │       │
  │       └── [nav线程] NavMeshHandle::AddObstacle → setPolyArea(63)
  │             │
  │             └── [回调] 成功 → 广播 cli_3d_addblock
  │
  └── 广播: Scene.SendToMapIns(ins_id, "cli_3d_addblock", {
            mapinsid = ins_id,
            blocks = { [tostring(ref)] = { BlockType=1, CenterX=..., ... } }
          })
```

### 2.2 删除阻挡

```
Lua 业务层：ObsMgr.Del(map_cfg_id, ref)
  │
  ├── E996.RemoveObstacle(map_id, ref, callback)
  │       │
  │       └── [nav线程] 恢复 area
  │
  └── 广播: Scene.SendToMapIns(ins_id, "cli_3d_delblock", {
            mapinsid = ins_id,
            blockid = { tostring(ref) }
          })
```

### 2.3 玩家进入地图

```
OnPlayerEnterMap(player, map_cfg_id, map_ins_id)
  │
  └── ObsMgr.SyncToPlayer(player, map_cfg_id)
        │
        └── E996.SendToClient(player, "cli_3d_addblock", {
                mapinsid = map_ins_id,
                blocks = <所有当前障碍物>
            })
```

> 进入地图时直接用 `cli_3d_addblock` 下发全量，不需要单独的 sync 消息。



## 3. Lua 服务端实现

### 3.1 ObsMgr.lua（新建 `release/e996_lua_ver_frame/svc_game/Map/ObsMgr.lua`）

```lua
local ObsMgr = {}
local Map3dBlockClientList = {}   -- { [mapInsId] = { [tostring(ref)] = cli_3d_blockinfo } }

-- MapMgr 进图时调用，取当前地图的 3D 障碍物列表
function ObsMgr.GetBlockList(mapInsId)
    return Map3dBlockClientList[mapInsId]
end

-- 添加圆柱障碍物 + 广播
function ObsMgr.AddCylinder(mapInsId, cx, cy, cz, radius, height, callback)
    local mapCfgId = ...   -- 通过 MapInsIdCache 查
    E996.AddObstacle(mapCfgId, cx, cy, cz, radius, height,
        function(result, ref)
            if result == 0 then
                Map3dBlockClientList[mapInsId] = Map3dBlockClientList[mapInsId] or {}
                Map3dBlockClientList[mapInsId][tostring(ref)] = {
                    BlockType=1, CenterX=cx, CenterY=cy, CenterZ=cz,
                    Radius=radius, Height=height, SizeX=0, SizeZ=0,
                }
                NetMsg.BroadGate("cli_3d_addblock", {
                    mapinsid = mapInsId, blocks = Map3dBlockClientList[mapInsId]
                })
            end
            if callback then callback(result, ref) end
        end)
end

-- 添加盒子障碍物 + 广播（类似 AddCylinder，BlockType=2）
function ObsMgr.AddBox(mapInsId, bmin_x, bmin_y, bmin_z, bmax_x, bmax_y, bmax_z, callback)
    -- ...
end

-- 删除障碍物 + 广播
function ObsMgr.DelObstacle(mapInsId, ref, callback)
    E996.RemoveObstacle(mapCfgId, ref,
        function(result)
            if result == 0 then
                Map3dBlockClientList[mapInsId][tostring(ref)] = nil
                NetMsg.BroadGate("cli_3d_delblock", {
                    mapinsid = mapInsId, blockid = { tostring(ref) }
                })
            end
            if callback then callback(result) end
        end)
end

return ObsMgr
```

### 3.2 MapMgr.lua 集成

```lua
-- 文件顶部新增
local ObsMgr = require("ObsMgr")

-- EnterMap() 中，2D 阻挡同步后紧跟 (line ~563):
local blocks3d = ObsMgr.GetBlockList(mapInsId)
if blocks3d ~= nil then
    NetMsg.BroadGate("cli_3d_addblock", { mapinsid = mapInsId, blocks = blocks3d })
end
```

---

## 4. 客户端显示

客户端收到协议后：

```
cli_3d_addblock → 解析 blocks map，逐个渲染半透明圆柱/盒子（红色或黄色提示）
cli_3d_delblock → 解析 blockid 列表，逐个移除对应渲染体
```

客户端不需要运行 navmesh 逻辑，服务端做权威寻路校验。

---

## 5. Msg_Svr 注册

需要在 `Msg_Svr_e996_lua_game_frame.lua` 中添加：

```lua
cli_3d_blockinfo = <hash>,
cli_3d_addblock  = <hash>,
cli_3d_delblock  = <hash>,
```

> hash 值由 `proto_generate.bat` 自动生成。

---

## 5. 生成 Msg_Svr 注册

需要在 `Msg_Svr_e996_lua_game_frame.lua` 中添加以下消息 ID：

```lua
cli_3d_obstacle_info = <hash>,
cli_3d_obstacle_add  = <hash>,
cli_3d_obstacle_adds = <hash>,
cli_3d_obstacle_del  = <hash>,
cli_3d_obstacle_dels = <hash>,
cli_3d_obstacle_sync = <hash>,
```

> hash 值由项目的 proto 工具自动生成（需要运行 `proto_generate.bat`）。

---

# 装备下线消失 Bug 分析（cache_main 覆盖修复）

## 1. Bug 现象

- 玩家穿上装备后下线再上线，装备栏为空
- Cache 日志显示 Item 数据被意外 DELETE

## 2. 复现步骤

```
1. 给玩家发装备（GM命令）
2. 玩家穿上装备（物品从背包 → 装备栏）
3. 等 30 秒
4. 下线
5. 上线
6. 装备栏是空的
```

## 3. 根本原因

### 3.1 引擎层的设计机制

```
Cache 数据库存储结构（t_data_1 表）：
┌───────────┬───────────┬──────────┬──────────┬──────────┐
│ main_owner│ sub_owner │ key_name │ val_type │ val_int  │
├───────────┼───────────┼──────────┼──────────┼──────────┤
│ PlayerId  │ ItemId    │ "Type"   │ INT      │ 6        │
│ PlayerId  │ ItemId    │ "CfgId"  │ INT      │ 41001    │
│ PlayerId  │ ItemId    │ "Name"   │ STR      │ "银戒指"  │
│ PlayerId  │ ItemId    │ "Where"  │ INT      │ 3        │
│ PlayerId  │ StoreId   │ "Items"  │ SYNCOBJ  │ ItemId   │ ← Store.Items 引用
└───────────┴───────────┴──────────┴──────────┴──────────┘
```

**对象引用 vs 对象数据是分开存储的**：Store.Items 只存一个引用（InsId），Item 自己的字段（Type/CfgId/Name 等）独立存储在同一个表里，用 `sub_owner = ItemId` 区分。

### 3.2 `FreeObj` 的级联删除机制

```cpp
// cache_main.cpp 第 863 行（修改前）
void cache_main::delete_handler(...) {
    // key_name 为空时 → 删除整个 sub_owner
    if (sub_owner) {
        // 生成 DELETE SQL：
        // DELETE FROM t_data_1 WHERE sub_owner = ItemId;
        // 这会删除 Item 的所有字段（Type/CfgId/Name/Where 等全部灭失）
        g_cache_mgr.async_write_db(self_sp,
            build_delete_sql(m_id, sub_owner), nullptr);
    }
}
```

**只要 `key_name` 为空，就删除该 `sub_owner` 的全部数据。这是引擎层的通用逻辑。**

### 3.3 Lua 层装备转移流程触发了 Delete

```
穿装备的完整流程：
┌──────────────────────────────────────────────────────────┐
│ 1. 物品在背包(Bag)中                                      │
│ 2. PutByInsID 从背包移到装备栏                             │
│ 3. DelItemFun → TakeItem   → 移除 Items 表引用              │
│ 4. DelItemFun → ItemFree   → FreeObj(Item, true)            │
│ 5. FreeObj(true) → Cache.Delete(Player, Item)                │
│ 6. delete_handler → build_delete_sql(sub_owner=ItemId)       │
│ 7. DELETE FROM t_data_1 WHERE sub_owner=ItemId ← 14个字段全丢 │
│ 8. AddItemToStore(Equips, Item)                              │
│ 9. AddByPos → SetInt("Where", 3) ← 只补了1个字段              │
│10. 下线→上线: LoginInit 读到 Type=nil → 跳过                  │
└──────────────────────────────────────────────────────────┘
```

### 3.4 时间线证据（来自 svc_cache 服务日志）

```
21:34:21 → INSERT: Type=6, CfgId=41001, Name="银戒指"  ...14个字段 ✅ 全部写入
21:34:23 → DELETE FROM t_data_1 WHERE sub_owner=494425031075  ❌ 全删
21:34:24 → INSERT: Where=3                                    ⚠️ 只剩1个字段
```

### 3.5 为什么只有 Where=3 幸存

`AddByPos` 在 Item 加入 Equips 后调用 `SetInt("Where", 3, SYNC_TO_CACHE)`，此时引擎能正常持久化这一个字段，但 Type/CfgId/Name 等没有对应的补写代码。

## 4. 修复方案

### 4.1 修改文件
`src/svc_cache/cache_main.cpp` 第 863 行

### 4.2 修改内容
注释掉 `build_delete_sql` 调用，sub_owner 清空时只清理内存，不删除数据库：

```cpp
// 修改前：
g_cache_mgr.async_write_db(self_sp, build_delete_sql(m_id, sub_owner), nullptr);

// 修改后：
// 注释掉：sub_owner 清空时不删 DB，由业务层自行管理数据生命周期
// g_cache_mgr.async_write_db(self_sp, build_delete_sql(m_id, sub_owner), nullptr);
```

### 4.3 修复原理

```
修复后的流程：
1. 物品在背包中
2. FreeObj 从内存清理 Item 引用
3. ★ 不再 DELETE 数据库数据 ★
4. Item 数据完整保留在 MySQL 中
5. AddItemToStore(Equips, Item) → Items 引用重新建立
6. AddByPos → SetInt("Where", 3) → 更新装备位置
7. 下线→上线: LoginInit 读到完整的 Type/CfgId/Name ✅
```

### 4.4 只改这一处够吗？

Yes。`FreeObj(Item, true)` 被调用时 `sub_owner = ItemId`、`key_name` 为空，必然走到 `delete_handler` 的第 831 行分支。注释掉这里的 `build_delete_sql` 就阻止了全部此类 Delete。

## 5. 关键文件

| 层级 | 文件 | 角色 |
|------|------|------|
| 引擎 C++ | `src/svc_cache/cache_main.cpp:863` | ★ 修复点：不再 delete sub_owner |
| 引擎 C++ | `src/svc_cache/cache_main.cpp:692` | `build_delete_sql` 生成 DELETE 语句 |
| 引擎 C++ | `src/svc_cache/cache_main.cpp:776` | `delete_handler` 处理删除消息 |
| Lua | `release/.../ItemStoreMgr.lua:465` | `ItemFree` → `FreeObj(Item, true)` |
| Lua | `release/.../ItemStoreMgr.lua:682` | `DelItemFun` → `TakeItem` + `ItemFree` |
| Lua | `release/.../ObjMgr.lua:212` | `FreeObj(InsObj, FromCache)` Lua 入口 |

## 6. 部署检查

1. 重新编译 `svc_cache` 项目（`cache_main.cpp` 是 svc_cache 的一部分）
2. 部署新编译的 Cache 服务
3. 重启 Cache + Game 服务
4. 测试：创建角色 → 穿装备 → 下线 → 上线，装备应在位

