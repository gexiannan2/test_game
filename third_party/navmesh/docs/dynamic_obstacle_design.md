# 动态阻挡（Dynamic Obstacle）设计文档 v2

> **主方案：setPolyArea（运行时标记 polygon area，无需重新烘焙）**

---

## 1. 方案对比：为什么选 setPolyArea 而不是 dtTileCache

### 1.1 两种方案的底层原理

```
setPolyArea（修改 polygon 属性）        dtTileCache（重建 tile 几何）
────────────────────────────────      ─────────────────────────────
修改 dtPoly.areaAndtype 字段内的      解压 tile cache layer →
area id（0~63）                       在 heightfield 上 rasterize 障碍物 →
                                      重新 contour/polyMesh/navMeshData →
                                      替换 dtNavMesh 中对应 tile 的 data
```

### 1.2 核心区别表

| 维度 | setPolyArea | dtTileCache |
|------|-------------|-------------|
| **离线工具改动** | 无需改动 ✅ | 需额外生成 tile cache data ❌ |
| **运行时数据结构** | 复用现有 NavMesh | 额外需要 dtTileCache（内存 +~30%） |
| **障碍物精度** | Polygon 级别 | 体素级别（精确到障碍物外形） |
| **圆柱体圆形边缘** | 阶梯状近似（覆盖到的 poly 全阻） | 精确圆形 |
| **添加耗时** | queryPolygons + setPolyArea，<1ms | addObstacle(<1ms) + 多次 update |
| **生效耗时** | 瞬间 | 每个受影响的 tile 需几 ms 重建 |
| **删除耗时** | 恢复 area，<1ms | removeObstacle(<1ms) + 多次 update |
| **同步/异步** | 天然同步 | 必须异步（重建 tile 不能同步等待） |
| **并发安全** | 安全（只改一个 uchar 字段） | 风险（物理替换 tile data） |
| **代码量** | ~200 行 | ~800 行 + 烘焙工具 |
| **已有依赖** | 全部已有（setPolyArea/queryPolygons） | 已有 dtTileCache.h/.cpp 但未集成 |
| **现有 .navmesh 兼容** | 100% 兼容 | 旧文件不支持，需重新烘焙 |

### 1.3 精度差异示意图

```
圆柱障碍物（半径 2m），落在 4m×4m 的 polygon 格子上：

        setPolyArea                         dtTileCache
    ┌────┬────┬────┬────┐              ┌────┬────┬────┬────┐
    │    │    │    │    │              │    │    │    │    │
    ├────┼────┼────┼────┤              ├────┼────┼────┼────┤
    │    │████│████│    │              │    │░░██│██░░│    │
    ├────┼────┼────┼────┤              ├────┼────┼────┼────┤
    │    │████│████│    │              │    │██░░│░░██│    │
    ├────┼────┼────┼────┤              ├────┼────┼────┼────┤
    │    │    │    │    │              │    │    │    │    │
    └────┴────┴────┴────┘              └────┴────┴────┴────┘

    覆盖 4 个 poly，全阻                  覆盖 4 个 poly，边缘精确挖空
    精度损失：~0.5-1 个 poly 的宽度      精度损失：~0
```

### 1.4 结论 & 建议

对于**服务器端寻路场景**：

- Polygon 本身已经很小（通常 2-5m），一个障碍物撑死覆盖几个 poly
- 寻路只需"绕得开"即可，不需要厘米级精度
- 同步、低延迟、高并发才是服务器的首要需求
- **setPolyArea 是更合理的选择**

dtTileCache 更适合客户端渲染端做精确的 NavMesh 碰撞显示，或对精度有极致要求的单机游戏。

---

## 2. 现有架构回顾

### 2.1 类层次

```
Navigation（单例管理器）
  └── KBEUnordered_map<std::string, NavigationHandlePtr> navhandles_

NavigationHandle（抽象基类，ref-counted）
  └── NavMeshHandle（NavMesh 实现）
      └── NavmeshLayer
          ├── dtNavMesh*      pNavmesh
          └── dtNavMeshQuery* pNavmeshQuery
```

### 2.2 已有可用的 Detour API

```cpp
// 查询范围内所有 polygon（GetHeight 已使用）
dtNavMeshQuery::queryPolygons(center, halfExtents, filter, &collector);

// 修改 polygon 的 area id（0~63，0=默认可行走）
dtNavMesh::setPolyArea(polyRef, area);
dtNavMesh::getPolyArea(polyRef, &area);

// 寻路 filter 控制
dtQueryFilter::setAreaCost(areaId, cost);   // 设置某 area 的通行代价
dtQueryFilter::setIncludeFlags(flags);       // 包含/排除 flag
dtQueryFilter::setExcludeFlags(flags);
```

---

## 3. 方案设计

### 3.1 核心思路

```
添加阻挡:
  1. 用 queryPolygons() 查出障碍物 AABB 范围内的所有 polygon
  2. 记录每个 polygon 的原始 area id
  3. 用 setPolyArea() 将这些 polygon 的 area 改为"不可行走"
  4. 返回一个唯一的障碍物 ref

删除阻挡:
  1. 根据 ref 找到之前记录的 polygon 列表
  2. 用 setPolyArea() 依次恢复原始 area id

寻路:
  无需改动 — 现有 dtQueryFilter 会对 area!=0 的 polygon 自动计算代价
  或: filter.setAreaCost(OBSTACLE_AREA, 999.f) 让特定 area 不可达
```

### 3.2 Polygon Area 规划

```cpp
#define OBSTACLE_AREA_ID  63    // 动态阻挡专用 area id（0~63，63 为最大值）
// 0 = 默认可行走区域（navmesh 烘焙时已使用）
// 1~62 = 保留给以后扩展
// 63 = 动态阻挡（被 setPolyArea 修改后）
```

寻路时 filter 设置：
```cpp
dtQueryFilter filter;
filter.setIncludeFlags(0xffff);
filter.setExcludeFlags(0);
filter.setAreaCost(OBSTACLE_AREA_ID, 99999.f);  // 极高代价 → 实际上不可达
```

---

## 4. 改动清单

```
src/svc_game/lib3d/navmesh/
├── nav/include/
│   ├── navigation_handle.h       [改] — 基类增加虚方法
│   └── navigation_mesh_handle.h   [改] — 障碍物管理结构体 + 方法声明
├── nav/src/
│   └── navigation_mesh_handle.cpp [改] — 核心实现
├── 3d_nav.h                       [改] — E996_API + e996_3d 命名空间
├── 3d_nav.cpp                     [改] — 同步/异步 API 实现
└── 3d_nav_lapi.cpp                [改] — Lua 绑定
```

**不涉及的文件**：
- `navmesh_baker.h/.cpp` — 不动
- `DetourTileCache.h/.cpp` — 不动
- `CMakeLists.txt` — 不动
- `nav/depend/` — 不动

---

## 5. 接口设计

### 5.1 NavigationHandle 基类（navigation_handle.h）

```cpp
class NavigationHandle : public RefCountable
{
public:
    // ... existing ...

    // 获取障碍物数量上限
    virtual int GetMaxObstacles() const = 0;

    // 添加圆柱障碍物: 返回 0 成功, <0 失败,
    //    outRef: 句柄（用于删除）, -2 表示 tilecache 不可用
    virtual int AddObstacle(int layer,
        const float* center, float radius, float height,
        std::uint32_t& outRef) = 0;

    // 添加盒子障碍物（AABB）
    virtual int AddBoxObstacle(int layer,
        const float* bmin, const float* bmax,
        std::uint32_t& outRef) = 0;

    // 删除障碍物
    virtual int RemoveObstacle(int layer, std::uint32_t ref) = 0;
};
```

### 5.2 NavMeshHandle（navigation_mesh_handle.h）

```cpp
class NavMeshHandle : public NavigationHandle
{
public:
    // ... existing ...

    // 内部障碍物记录结构
    struct ObstacleRecord {
        std::uint32_t ref;              // 唯一句柄
        std::vector<dtPolyRef> polys;   // 影响的 polygon 列表
        std::vector<unsigned char> origAreas; // 原始 area id
        float boundMin[3];              // 障碍物 AABB min
        float boundMax[3];              // 障碍物 AABB max
    };

    // 障碍物管理
    int GetMaxObstacles() const override { return kMaxObstacles; }
    int AddObstacle(int layer, const float* center, float radius, 
                    float height, std::uint32_t& outRef) override;
    int AddBoxObstacle(int layer, const float* bmin, const float* bmax,
                       std::uint32_t& outRef) override;
    int RemoveObstacle(int layer, std::uint32_t ref) override;

private:
    static const int kMaxObstacles = 256;
    std::vector<ObstacleRecord> obstacles_;
    std::uint32_t nextObstacleRef_ = 1;

    // 内部: 根据 AABB 查询并标记受影响的 polygon
    int MarkPolygons(int layer, const float* bmin, const float* bmax,
                     std::vector<dtPolyRef>& outPolys,
                     std::vector<unsigned char>& outOrigAreas);
};
```

### 5.3 核心实现（navigation_mesh_handle.cpp）

```cpp
#define E996_OBSTACLE_AREA  63

int NavMeshHandle::MarkPolygons(int layer, const float* bmin, const float* bmax,
    std::vector<dtPolyRef>& outPolys, std::vector<unsigned char>& outOrigAreas)
{
    auto it = navmeshLayer.find(layer);
    if (it == navmeshLayer.end()) return -1;

    dtNavMeshQuery* query = it->second.pNavmeshQuery;
    dtNavMesh*       mesh  = it->second.pNavmesh;

    // 计算查询中心 + 半边长
    float center[3] = {
        (bmin[0] + bmax[0]) * 0.5f,
        (bmin[1] + bmax[1]) * 0.5f,
        (bmin[2] + bmax[2]) * 0.5f
    };
    float halfExt[3] = {
        (bmax[0] - bmin[0]) * 0.5f + 0.5f,  // +0.5 容差确保覆盖边界 poly
        (bmax[1] - bmin[1]) * 0.5f + 0.5f,
        (bmax[2] - bmin[2]) * 0.5f + 0.5f
    };

    dtQueryFilter filter;
    filter.setIncludeFlags(0xffff);
    filter.setExcludeFlags(0);

    // 查询范围内的所有 polygon
    static const int MAX_POLYS = 512;
    dtPolyRef polys[MAX_POLYS];
    int polyCount = 0;
    dtStatus st = query->queryPolygons(center, halfExt, &filter,
        polys, &polyCount, MAX_POLYS);
    if (dtStatusFailed(st)) return -2;

    // 记录原始 area 并修改
    for (int i = 0; i < polyCount; ++i) {
        unsigned char origArea;
        mesh->getPolyArea(polys[i], &origArea);
        if (origArea == E996_OBSTACLE_AREA) continue;  // 已被占用，跳过
        mesh->setPolyArea(polys[i], E996_OBSTACLE_AREA);
        outPolys.push_back(polys[i]);
        outOrigAreas.push_back(origArea);
    }
    return outPolys.size() > 0 ? 0 : -3;  // -3 = 没有覆盖到任何 poly
}

int NavMeshHandle::AddObstacle(int layer, const float* center,
    float radius, float height, std::uint32_t& outRef)
{
    float bmin[3] = {center[0]-radius, center[1],          center[2]-radius};
    float bmax[3] = {center[0]+radius, center[1]+height,   center[2]+radius};

    ObstacleRecord rec;
    rec.ref = nextObstacleRef_++;
    std::memcpy(rec.boundMin, bmin, sizeof(bmin));
    std::memcpy(rec.boundMax, bmax, sizeof(bmax));

    int ret = MarkPolygons(layer, bmin, bmax, rec.polys, rec.origAreas);
    if (ret < 0) return ret;

    obstacles_.push_back(rec);
    outRef = rec.ref;
    return 0;
}

int NavMeshHandle::AddBoxObstacle(int layer, const float* bmin,
    const float* bmax, std::uint32_t& outRef)
{
    ObstacleRecord rec;
    rec.ref = nextObstacleRef_++;
    std::memcpy(rec.boundMin, bmin, sizeof(float)*3);
    std::memcpy(rec.boundMax, bmax, sizeof(float)*3);

    int ret = MarkPolygons(layer, bmin, bmax, rec.polys, rec.origAreas);
    if (ret < 0) return ret;

    obstacles_.push_back(rec);
    outRef = rec.ref;
    return 0;
}

int NavMeshHandle::RemoveObstacle(int layer, std::uint32_t ref)
{
    auto it = navmeshLayer.find(layer);
    if (it == navmeshLayer.end()) return -1;

    dtNavMesh* mesh = it->second.pNavmesh;

    for (auto obs = obstacles_.begin(); obs != obstacles_.end(); ++obs) {
        if (obs->ref != ref) continue;

        // 恢复所有 polygon 的原始 area
        for (size_t i = 0; i < obs->polys.size(); ++i) {
            mesh->setPolyArea(obs->polys[i], obs->origAreas[i]);
        }
        obstacles_.erase(obs);
        return 0;
    }
    return -2;  // 未找到
}
```

### 5.4 3d_nav.h 的 E996_API 层

```cpp
// ── 动态阻挡 C API ──

// 同步: 添加圆柱障碍物
E996_API int e996_obstacle_add(std::uint32_t map_cfg_id,
    float cx, float cy, float cz,     // 障碍物中心点 (navmesh 坐标系)
    float radius, float height,       // 半径 & 高度
    std::uint32_t* out_ref);          // [out] 障碍物句柄

// 同步: 添加盒子障碍物
E996_API int e996_obstacle_add_box(std::uint32_t map_cfg_id,
    float min_x, float min_y, float min_z,
    float max_x, float max_y, float max_z,
    std::uint32_t* out_ref);

// 同步: 删除障碍物
E996_API int e996_obstacle_remove(std::uint32_t map_cfg_id, std::uint32_t ref);

// ── 异步: 投递到 nav 线程执行 ──

using nav_obstacle_add_cb_lbd = std::function<void(int result, std::uint32_t ref)>;
using nav_obstacle_add_cb_c  = e996::lbd_to_cfunc_t<nav_obstacle_add_cb_lbd>;

using nav_obstacle_cb_lbd = std::function<void(int result)>;
using nav_obstacle_cb_c  = e996::lbd_to_cfunc_t<nav_obstacle_cb_lbd>;

E996_API int e996_obstacle_add_async(std::uint32_t map_cfg_id,
    float cx, float cy, float cz, float radius, float height,
    nav_obstacle_add_cb_c cb, std::uint64_t cb_id);

E996_API int e996_obstacle_add_box_async(std::uint32_t map_cfg_id,
    float min_x, float min_y, float min_z,
    float max_x, float max_y, float max_z,
    nav_obstacle_add_cb_c cb, std::uint64_t cb_id);

E996_API int e996_obstacle_remove_async(std::uint32_t map_cfg_id,
    std::uint32_t ref,
    nav_obstacle_cb_c cb, std::uint64_t cb_id);
```

### 5.5 3d_nav.cpp 同步实现

```cpp
// 同步添加：在调用方线程直接操作 g_navHandle
E996_API int e996_obstacle_add(std::uint32_t map_cfg_id,
    float cx, float cy, float cz,
    float radius, float height,
    std::uint32_t* out_ref)
{
    if (!g_navHandle) return -3;

    float center[3] = {cx, cy, cz};
    std::uint32_t ref = 0;
    int ret = g_navHandle->AddObstacle(
        static_cast<int>(map_cfg_id), center, radius, height, ref);
    if (out_ref) *out_ref = ref;
    return ret;
}

E996_API int e996_obstacle_remove(std::uint32_t map_cfg_id, std::uint32_t ref)
{
    if (!g_navHandle) return -3;
    return g_navHandle->RemoveObstacle(static_cast<int>(map_cfg_id), ref);
}
```

### 5.6 3d_nav.cpp 异步实现

```cpp
E996_API int e996_obstacle_add_async(std::uint32_t map_cfg_id,
    float cx, float cy, float cz,
    float radius, float height,
    nav_obstacle_add_cb_c cb, std::uint64_t cb_id)
{
    if (!cb) return -1;

    struct Result { int ret; uint32_t ref; };
    auto* r = new Result{0, 0};

    auto& s = e996_3d_nav::get_storage();
    e996::post_threadwork(s.thread, "nav_obstacle_add",
        [=]() {
            r->ret = e996_obstacle_add(map_cfg_id, cx, cy, cz,
                radius, height, &r->ref);
        },
        [cb, cb_id, r]() {
            cb(cb_id, r->ret, r->ref);
            delete r;
        });
    return 0;
}

// e996_obstacle_remove_async 类似
```

### 5.7 e996_3d 命名空间（C++ 便捷层）

```cpp
namespace e996_3d
{
    // ── 同步 ──
    inline int add_obstacle(uint32_t map_id, float cx, float cy, float cz,
        float radius, float height, uint32_t& out_ref)
    {
        return e996_obstacle_add(map_id, cx, cy, cz, radius, height, &out_ref);
    }
    inline int remove_obstacle(uint32_t map_id, uint32_t ref)
    {
        return e996_obstacle_remove(map_id, ref);
    }

    // ── 异步 ──
    inline void add_obstacle_async(uint32_t map_id,
        float cx, float cy, float cz, float radius, float height,
        nav_obstacle_add_cb_lbd cb)
    {
        e996_obstacle_add_async(map_id, cx, cy, cz, radius, height,
            e996::get_lambda_cb<nav_obstacle_add_cb_lbd>(),
            e996::alloc_lambda_id<nav_obstacle_add_cb_lbd>(cb));
    }
    inline void remove_obstacle_async(uint32_t map_id,
        uint32_t ref, nav_obstacle_cb_lbd cb)
    {
        e996_obstacle_remove_async(map_id, ref,
            e996::get_lambda_cb<nav_obstacle_cb_lbd>(),
            e996::alloc_lambda_id<nav_obstacle_cb_lbd>(cb));
    }
}
```

### 5.8 Lua 绑定（3d_nav_lapi.cpp）

```cpp
// E996.AddObstacle(map_id, cx, cy, cz, radius, height, callback)
//   callback(result, ref)
//     result: 0=成功, -1=参数错, -2=底层失败, -3=navmesh未加载
static int lua_add_obstacle(lua_State* L)
{
    auto map_id = (uint32_t)luaL_checkinteger(L, 1);
    float cx = (float)luaL_checknumber(L, 2);
    float cy = (float)luaL_checknumber(L, 3);
    float cz = (float)luaL_checknumber(L, 4);
    float r  = (float)luaL_checknumber(L, 5);
    float h  = (float)luaL_checknumber(L, 6);
    if (!lua_isfunction(L, 7)) return luaL_argerror(L, 7, "expect function");

    int cb_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    e996_3d::add_obstacle_async(map_id, cx, cy, cz, r, h,
        [L, cb_ref](int result, uint32_t ref) {
            alua::call(cb_ref, result, ref);
            luaL_unref(L, LUA_REGISTRYINDEX, cb_ref);
        });
    return 0;
}

// E996.RemoveObstacle(map_id, ref, callback)
static int lua_remove_obstacle(lua_State* L)
{
    auto map_id = (uint32_t)luaL_checkinteger(L, 1);
    uint32_t ref = (uint32_t)luaL_checkinteger(L, 2);
    if (!lua_isfunction(L, 3)) return luaL_argerror(L, 3, "expect function");

    int cb_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    e996_3d::remove_obstacle_async(map_id, ref,
        [L, cb_ref](int result) {
            alua::call(cb_ref, result);
            luaL_unref(L, LUA_REGISTRYINDEX, cb_ref);
        });
    return 0;
}

// E996.AddBoxObstacle(map_id, min_x, min_y, min_z, max_x, max_y, max_z, callback)
// 类似 lua_add_obstacle，省略

// 注册
static const luaL_Reg funcs[] = {
    // ... existing ...
    {"AddObstacle",     lua_add_obstacle},
    {"AddBoxObstacle",  lua_add_box_obstacle},
    {"RemoveObstacle",  lua_remove_obstacle},
    {NULL, NULL}
};
```

---

## 6. 调用示例

### 6.1 C++ 同步

```cpp
uint32_t ref;
e996_3d::add_obstacle(101, 100.f, 0.f, 200.f, 3.0f, 5.0f, ref);
// 寻路已自动生效，障碍物范围内的 polygon 已被标记为不可行走

// 删除
e996_3d::remove_obstacle(101, ref);
```

### 6.2 C++ 异步（跨 SO）

```cpp
e996_3d::add_obstacle_async(101, 100.f, 0.f, 200.f, 3.0f, 5.0f,
    [](int result, uint32_t ref) {
        if (result == 0) {
            // 添加成功，寻路自动绕开
        }
    });
```

### 6.3 Lua

```lua
E996.AddObstacle(101, 100, 0, 200, 3.0, 5.0, function(result, ref)
    if result == 0 then
        print("Obstacle added, ref=" .. ref)
        -- 寻路已自动生效
    end
end)

-- 删除
E996.RemoveObstacle(101, ref, function(result)
    print("Obstacle removed")
end)
```

---

## 7. 寻路 Filter 配置

为了让被标记为 `OBSTACLE_AREA(63)` 的 polygon 被寻路绕开，需要确认 filter 配置。当前代码中 filter 的用法：

```cpp
dtQueryFilter filter;
filter.setIncludeFlags(0xffff);
filter.setExcludeFlags(0);
```

Detour 默认 `dtQueryFilter::getCost()` 会用 `m_areaCost[poly->getArea()]` 计算代价，
默认 `m_areaCost[0] = 1.0`，其他 area 的默认值也是 1.0。

**需要增加一行**，将 OBSTACLE_AREA 的代价设为极高：

```cpp
filter.setAreaCost(63, 99999.f);   // 极高代价 → 寻路几乎不可能经过
```

这个改动只需在 `do_raycast_check()` 和 `do_find_path()` 的 filter 创建处加一行即可。

---

## 8. 并发安全

`setPolyArea()` 只修改 `dtPoly::areaAndtype` 字段（1 个 unsigned char），是原子性的值写入。
`findPath()` / `raycast()` 读取 `poly->getArea()` 时不会受写入影响（读到一个完整的新值或旧值，不会读到中间状态）。

**无需加锁**。

---

## 9. 改动风险评估

| 风险项 | 级别 | 缓解 |
|--------|------|------|
| filter 未配置 OBSTACLE_AREA 代价 | 低 | 检查现有 filter 使用点，统一加上 setAreaCost(63, 99999) |
| 障碍物数量上限 | 低 | kMaxObstacles=256，典型场景足够 |
| 跨 SO 回调 | 低 | 沿用已验证的 lbd_to_cfunc_t 模式 |
| 现有寻路性能退化 | 无 | setAreaCost 调用是一次数组赋值，无额外开销 |
| 两个代码库同步 | 低 | 总量 ~200 行，易于同步到 common/e996_navmesh/ |

---

## 10. 不在本次范围的

- OBB 旋转盒 Lua API（可后续补充）
- 批量障碍物操作
- dtTileCache 方案（保留作为远期高级选项）
- `common/e996_navmesh/` 的同步改动（待本方案验证后）
