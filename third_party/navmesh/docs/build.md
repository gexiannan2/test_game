# 构建说明

## 快速构建

```bash
cd /mnt/d/e996_navmesh
cmake -S . -B build
cmake --build build -j4
```

默认构建类型为 `Debug`，并显式追加 `-O0 -g`，便于在 VS Code 或 `gdb` 中调试。

## 主要产物

```text
build/navmesh_bake_from_obj
build/navmesh_dump
build/navmesh_query_demo
build/navmesh_export_json
build/navmesh_export_obj
build/navmesh_path_compare
build/navmesh_triangle_path_demo
build/navmesh_smooth_path_demo
build/navigation_load_by_name_demo
Bin/lib/libnavigation.so
```

## 工具用途

- `navmesh_bake_from_obj`：从 OBJ 场景几何生成 `.navmesh`，可选输出 SVG 顶视图。
- `navmesh_dump`：读取 `.navmesh` 并输出文件头和 tile 摘要。
- `navmesh_query_demo`：加载 `.navmesh` 并执行最小运行时查询。
- `navmesh_export_json`：导出可读的多边形 JSON。
- `navmesh_export_obj`：导出导航网格细节三角形 OBJ。
- `navmesh_path_compare`：对比路径结果。
- `navmesh_triangle_path_demo`：观察多边形或三角形路径行为。
- `navmesh_smooth_path_demo`：检查平滑路径效果。
- `navigation_load_by_name_demo`：验证 `Navigation::loadNavigation(name)` 加载本地资源。

## 常用验证

```bash
./build/navmesh_dump output_101_navmesh/101_web_like.navmesh
./build/navmesh_query_demo output_101_navmesh/101_web_like.navmesh
./build/navigation_load_by_name_demo 101_nav 0 0 0 10 0 10 output_101_navmesh/path.txt
```

## 清理重建

```bash
rm -rf build
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS_DEBUG="-O0 -g" \
  -DCMAKE_CXX_FLAGS_DEBUG="-O0 -g"
cmake --build build -j4
```

## 说明

- 当前构建只包含 3D `NavMesh` 路线。
- 二维瓦片和二维栅格相关目标已经从构建入口移除。
- Windows 下仍保留 Visual Studio 工程文件，但当前推荐以 CMake 为准。
