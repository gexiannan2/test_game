# navigation 项目模块说明

## 项目定位

当前工程是一个面向 3D 场景的导航库，只保留 `NavMesh` 路线。运行时以
`.navmesh` 为导航资源，内部通过 Detour 执行路径查询、射线查询、贴地和最近点查询。

## 模块总览

| 模块 | 文件 | 作用 |
| --- | --- | --- |
| 导航入口 | `navigation.h/.cpp` | 按名称加载、缓存、查找和释放导航实例 |
| 导航接口 | `navigation_handle.h/.cpp` | 定义统一查询接口 |
| NavMesh 运行时 | `navigation_mesh_handle.h/.cpp` | 加载 `.navmesh` 并执行 Detour 查询 |
| NavMesh 烘焙 | `navmesh_baker.h/.cpp` | 从 OBJ 场景几何生成 `.navmesh` |
| Recast 核心 | `Recast*.h/.cpp` | 体素化、区域划分、轮廓和多边形网格生成 |
| Detour 核心 | `Detour*.h/.cpp` | 导航网格数据结构、路径查询和射线查询 |
| 基础依赖 | `depend/common` | 平台、类型、内存、智能指针和工具函数 |
| 数学依赖 | `depend/math` | `Position3D`、向量和基础几何类型 |
| 资源依赖 | `depend/resmgr` | 路径匹配、文件打开和目录扫描 |
| 辅助依赖 | `depend/g3dlite` | 历史 KBEngine 依赖裁剪 |
| 工具 | `tools/` | 烘焙、dump、查询、导出和路径对比 |

## 运行时调用关系

```text
业务调用
   |
   v
Navigation::loadNavigation(name)
   |
   v
NavMeshHandle::create(name)
   |
   +-- 查找 res/<name>.navmesh 等本地资源路径
   +-- 读取 NavMeshSetHeader / NavMeshTileHeader
   +-- 恢复 dtNavMesh
   +-- 初始化 dtNavMeshQuery
   |
   v
findStraightPath / raycast / GetHeight / GetNearPos / Intersects
```

## NavMesh 查询能力

- `findStraightPath`：根据起点和终点查找直线路径点。
- `raycast`：沿导航网格检测可通行射线。
- `raycastNear`：使用更大的最近多边形范围执行射线检测。
- `raycastAlong`：沿导航面移动并返回实际可到达位置。
- `GetHeight`：查询指定位置对应的导航面高度。
- `GetNearPos`：查找离输入点最近的可行走位置。
- `Intersects`：做路径和导航高度关系的相交判断。

## 资源格式

当前 `.navmesh` 文件使用项目自定义封装：

```text
NavMeshSetHeader
NavMeshTileHeader 0
Tile Data 0
NavMeshTileHeader 1
Tile Data 1
...
```

Tile payload 仍是 Detour 生成的数据。运行时会逐个 tile 调用 `dtNavMesh::addTile`
恢复可查询的导航网格。

## 当前边界

- 当前工程不再包含 2D 瓦片或二维栅格寻路实现。
- `Navigation::loadNavigation(name)` 只创建 `NavMeshHandle`。
- 后续升级 KBEngine 代码时，应保留当前运行时接口和本地 `.navmesh` 加载规则。
