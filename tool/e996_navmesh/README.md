# navigation

`navigation` 是一个面向 3D 场景的 C++ 导航工程。当前工程只保留
`NavMesh` 路线，核心基于 Recast/Detour，目标是支持类似动作 RPG 场景中的
服务端寻路、射线检测、贴地高度查询、最近可行点查询和导航网格烘焙验证。

## 当前能力

- 基于仓库内源码直接构建 Recast/Detour。
- 加载项目自定义封装格式的 `.navmesh` 文件。
- 支持 `findStraightPath`、`raycast`、`raycastNear`、`raycastAlong`、
  `GetHeight`、`GetNearPos`、`Intersects` 等运行时查询。
- 支持从 `OBJ` 场景几何烘焙生成 `.navmesh`。
- 提供 dump、查询、导出 JSON、导出 OBJ、路径对比等调试工具。

## 目录说明

- `CMakeLists.txt`：当前推荐构建入口。
- `navigation.h/.cpp`：导航对象加载、缓存、查找和释放入口。
- `navigation_handle.h/.cpp`：导航运行时接口定义。
- `navigation_mesh_handle.h/.cpp`：`.navmesh` 加载和 Detour 查询实现。
- `Recast*.h/.cpp`：导航网格烘焙核心。
- `Detour*.h/.cpp`：导航网格运行时查询核心。
- `navmesh_baker.h/.cpp`：从 OBJ 烘焙 `.navmesh` 的封装。
- `depend/`：当前工程所需的 KBEngine 基础依赖裁剪版。
- `tools/`：烘焙、检查、查询和导出工具。
- `test/`：最小运行时加载示例。
- `docs/`：构建、模块和对比说明。
- `res/`：示例运行时导航资源。

## 构建

```bash
cmake -S . -B build
cmake --build build -j4
```

主要产物：

- `build/navmesh_bake_from_obj`
- `build/navmesh_dump`
- `build/navmesh_query_demo`
- `build/navmesh_export_json`
- `build/navmesh_export_obj`
- `build/navmesh_path_compare`
- `build/navmesh_triangle_path_demo`
- `build/navmesh_smooth_path_demo`
- `build/navigation_load_by_name_demo`
- `Bin/lib/libnavigation.so`

## 示例

从 `101.obj` 烘焙导航网格：

```bash
./build/navmesh_bake_from_obj \
  ./101.obj \
  output_101_navmesh/101_web_like.navmesh \
  output_101_navmesh/101_web_like.svg
```

检查 `.navmesh`：

```bash
./build/navmesh_dump output_101_navmesh/101_web_like.navmesh
```

运行最小查询验证：

```bash
./build/navmesh_query_demo output_101_navmesh/101_web_like.navmesh
```

按名称加载示例资源：

```bash
./build/navigation_load_by_name_demo 101_nav 0 0 0 10 0 10 output_101_navmesh/path.txt
```

## 说明

- 当前工程只保留 3D `NavMesh` 路线。
- 2D 瓦片、二维栅格和相关示例工具已经从源码和构建入口中移除。
- `.navmesh` 不是裸 Detour 数据，而是项目自定义二进制封装，内部 tile payload
  仍为 Detour 生成的数据。

## License

仓库中保留了上游与历史代码使用的 [License.txt](License.txt)。如需对外分发或二次集成，
建议结合实际代码来源再次核对许可证。
