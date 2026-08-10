# `.navmesh` layout

```text
+---------------------+
| NavMeshSetHeader    |
+---------------------+
| NavMeshTileHeader 0 |
+---------------------+
| Tile Data 0         |  <-- Detour tile blob created by dtCreateNavMeshData()
+---------------------+
| NavMeshTileHeader 1 |
+---------------------+
| Tile Data 1         |
+---------------------+
| ...                 |
+---------------------+
```

`NavMeshSetHeader`

- `int version`
- `int tileCount`
- `dtNavMeshParams params`

`NavMeshTileHeader`

- `dtTileRef tileRef`
- `int dataSize`

Each tile payload begins with a Detour `dtMeshHeader`, followed by packed arrays for
vertices, polygons, links, detail meshes, detail vertices, detail triangles, BV tree,
and off-mesh connections.

# Tooling

当前 `tools/` 目录里和 navmesh 相关的几个小工具如下：

- `navmesh_bake_from_obj`
  - 输入 `obj`
  - 输出当前工程可直接加载的 `.navmesh`
  - 同时导出一个顶视图 `svg`
- `navmesh_dump`
  - 读取 `.navmesh`
  - 打印文件头、tile 头和 `dtMeshHeader` 摘要
- `navmesh_query_demo`
  - 读取 `.navmesh`
  - 运行一组最小查询
  - 包括 `findPath`、`findStraightPath`、`moveAlongSurface`、`raycast`、`findDistanceToWall`
- `navmesh_export_json`
  - 读取 `.navmesh`
  - 导出明文 `json`
  - 包含 tile、polygon、顶点、centroid、flags、area 等信息

## Build

先重新生成并编译当前工程：

```bash
cmake -S /mnt/d/e996_navmesh -B /mnt/d/e996_navmesh/build_current
cmake --build /mnt/d/e996_navmesh/build_current --parallel
```

如果只想单独编某个工具，例如：

```bash
cmake --build /mnt/d/e996_navmesh/build_current --target navmesh_bake_from_obj --parallel
cmake --build /mnt/d/e996_navmesh/build_current --target navmesh_query_demo --parallel
cmake --build /mnt/d/e996_navmesh/build_current --target navmesh_export_json --parallel
```

## Typical Flow

推荐的排查和验证顺序是：

1. `obj -> .navmesh + .svg`
2. `.navmesh -> dump`
3. `.navmesh -> runtime query`
4. `.navmesh -> json`

## Example Commands

下面这组命令就是当前仓库里已经验证通过的 `101.obj` 示例。

### 1. Bake from OBJ

```bash
/mnt/d/e996_navmesh/build_current/navmesh_bake_from_obj \
  /mnt/c/Users/admin/Downloads/101.obj \
  /mnt/d/e996_navmesh/output_101_navmesh/101_formal.navmesh \
  /mnt/d/e996_navmesh/output_101_navmesh/101_formal.svg
```

输出：

- `101_formal.navmesh`
- `101_formal.svg`

### 2. Dump Binary Header Info

```bash
/mnt/d/e996_navmesh/build_current/navmesh_dump \
  /mnt/d/e996_navmesh/output_101_navmesh/101_formal.navmesh
```

这个工具适合快速确认：

- 文件版本
- tile 数量
- `polyCount`
- `vertCount`
- `detailTriCount`
- `bmin/bmax`
- 数据尺寸是否匹配

### 3. Run Runtime Query Demo

```bash
/mnt/d/e996_navmesh/build_current/navmesh_query_demo \
  /mnt/d/e996_navmesh/output_101_navmesh/101_formal.navmesh
```

这个工具会直接在当前 `.navmesh` 上跑：

- `findPath`
- `findStraightPath`
- `moveAlongSurface`
- `raycast`
- `findDistanceToWall`
- `getPolyHeight`

### 4. Export Polygon JSON

```bash
/mnt/d/e996_navmesh/build_current/navmesh_export_json \
  /mnt/d/e996_navmesh/output_101_navmesh/101_formal.navmesh \
  /mnt/d/e996_navmesh/output_101_navmesh/101_polygons.json
```

输出的 `json` 里包含：

- `tile_count`
- `walkable_polygon_count`
- 每个 tile 的 `bmin` / `bmax`
- 每个 polygon 的：
  - `poly_index`
  - `poly_ref`
  - `area`
  - `flags`
  - `vert_count`
  - `detail_tri_count`
  - `centroid`
  - `vertices`

## Notes

- `.navmesh` 是二进制文件，不能直接用文本编辑器明文查看。
- 如果要看图形，优先看 `svg`。
- 如果要看结构化文本，优先用 `navmesh_export_json`。
- 如果要看运行时查询是否正常，优先用 `navmesh_query_demo`。
- 生成产物不要放 `/tmp`，建议放在工程目录下，例如：
  - `/mnt/d/e996_navmesh/output_101_navmesh/`

## `101.obj` -> navmesh 详细流程

这一节专门梳理当前仓库里 `101.obj` 从原始场景几何到最终 `.navmesh` 的完整链路。这里要先区分两件事：

- “当前代码实际怎么烘焙”
- “仓库里已经存在哪些历史样例文件”

这两个层面在 `output_101_navmesh/` 里是混在一起的，所以如果不先拆开，很容易被旧文件名误导。

### 1. 输入源文件

- 当前烘焙入口使用的是仓库根目录下的 `101.obj`
- `output_101_navmesh/101.obj` 只是这份输入文件的拷贝，内容相同
- 我在本地于 `2026-04-23` 用当前工具复跑时，`101.obj` 被解析为：
  - `38042` 个顶点
  - `17178` 个三角形

### 2. 命令入口长什么样

实际入口程序是 `tools/navmesh_bake_from_obj.cpp`，命令格式是：

```bash
./build/navmesh_bake_from_obj <input.obj> <output.navmesh> [output.svg] [options]
```

它做的事情比较直接：

- 第 1 个参数是输入 `obj`
- 第 2 个参数是输出 `.navmesh`
- 第 3 个参数如果不是 `--xxx`，就会被当作输出 `svg`
- 后面的 `--cell-size`、`--agent-radius` 等参数会覆盖默认烘焙配置

如果你只是想从 `101.obj` 生成一份“按当前代码默认参数”的 navmesh，建议直接用一个新名字，避免和目录里的历史样例混淆：

```bash
./build/navmesh_bake_from_obj \
  ./101.obj \
  output_101_navmesh/101_default.navmesh \
  output_101_navmesh/101_default.svg
```

我在本地于 `2026-04-23` 复跑这条默认命令时，输出摘要是：

- `tiles=1`
- `walkablePolys=4273`
- `walkableHeightVoxels=10`
- `walkableClimbVoxels=2`
- `walkableRadiusVoxels=2`

如果你想烘一份更细粒度、尽量保留小障碍和细通道的版本，也可以显式传一组更高分辨率参数，例如：

```bash
./build/navmesh_bake_from_obj \
  ./101.obj \
  output_101_navmesh/101_web_like_current.navmesh \
  output_101_navmesh/101_web_like_current.svg \
  --cell-size 0.2 \
  --cell-height 0.1 \
  --agent-height 2.0 \
  --agent-radius 0.4 \
  --agent-max-climb 1.2 \
  --agent-max-slope 50 \
  --region-min-size 4 \
  --region-merge-size 12 \
  --edge-max-len 12 \
  --edge-max-error 1.1 \
  --verts-per-poly 6 \
  --detail-sample-dist 4 \
  --detail-sample-max-error 0.8
```

我在本地于 `2026-04-23` 用当前代码复跑这组命令时，输出摘要是：

- `tiles=1`
- `walkablePolys=6529`
- `walkableHeightVoxels=20`
- `walkableClimbVoxels=12`
- `walkableRadiusVoxels=2`

这个结果和仓库里现成的历史文件 `output_101_navmesh/101_web_like.navmesh` 并不完全相同，所以后者更适合作为“历史样例”，不要把它当成“当前命令一定逐字节复现的标准答案”。

### 3. 代码内部到底做了什么

下面是从 `101.obj` 到 `.navmesh` 的真实代码链路。

#### 3.1 读 OBJ

`navmesh_baker.cpp` 里的 `loadObj()` 会逐行读取文件：

- 读到 `v` 行时，收集顶点 `x y z`
- 读到 `f` 行时，解析面索引
- 如果一个面不是三角形，会用扇形拆分方式把它三角化

所以从这一步结束后，程序手里拿到的是：

- `verts`
- `tris`

也就是标准的“三角面片场景网格”。

#### 3.2 把世界单位参数换成 Recast 体素参数

接下来 `buildSoloNavMesh()` 会先算包围盒 `bmin/bmax`，然后把 `NavmeshBakeSettings` 转成 `rcConfig`。

这里最关键的是 3 个换算：

- `walkableHeight = ceil(agentHeight / cellHeight)`
- `walkableClimb = floor(agentMaxClimb / cellHeight)`
- `walkableRadius = ceil(agentRadius / cellSize)`

也就是说，命令行里传入的是世界单位参数，但真正进 Recast 之前，会先折算成体素单位。

当前代码默认参数是：

- `cellSize=0.3`
- `cellHeight=0.2`
- `agentHeight=2.0`
- `agentRadius=0.5`
- `agentMaxClimb=0.4`
- `agentMaxSlope=45.0`
- `regionMinSize=8.0`
- `regionMergeSize=20.0`
- `edgeMaxLen=12.0`
- `edgeMaxError=1.3`
- `vertsPerPoly=6`
- `detailSampleDist=6.0`
- `detailSampleMaxError=1.0`

#### 3.2.1 以你这条 `101_web_like` 命令为例，`cfg` 里每个关键字段从哪来

如果使用的是下面这条命令：

```bash
./build/navmesh_bake_from_obj \
  ./101.obj \
  output_101_navmesh/101_web_like.navmesh \
  output_101_navmesh/101_web_like.svg \
  --cell-size 0.2 \
  --cell-height 0.1 \
  --agent-height 2.0 \
  --agent-radius 0 \
  --agent-max-climb 1.2 \
  --agent-max-slope 50 \
  --region-min-size 4 \
  --region-merge-size 12 \
  --edge-max-len 12 \
  --edge-max-error 1.1 \
  --verts-per-poly 6 \
  --detail-sample-dist 4 \
  --detail-sample-max-error 0.8
```

那么 `buildSoloNavMesh()` 里关键参数来源可以拆成三类：

第一类：直接等于命令行参数

- `cfg.cs = settings.cellSize = 0.2`
- `cfg.ch = settings.cellHeight = 0.1`
- `cfg.walkableSlopeAngle = settings.agentMaxSlope = 50`
- `cfg.maxSimplificationError = settings.edgeMaxError = 1.1`
- `cfg.maxVertsPerPoly = settings.vertsPerPoly = 6`

第二类：由命令行参数换算成体素单位

- `cfg.walkableHeight = ceil(2.0 / 0.1) = 20`
- `cfg.walkableClimb = floor(1.2 / 0.1) = 12`
- `cfg.walkableRadius = ceil(0 / 0.2) = 0`
- `cfg.maxEdgeLen = 12 / 0.2 = 60`
- `cfg.minRegionArea = 4^2 = 16`
- `cfg.mergeRegionArea = 12^2 = 144`
- `cfg.detailSampleDist = 0.2 * 4 = 0.8`
- `cfg.detailSampleMaxError = 0.1 * 0.8 = 0.08`

第三类：不是命令行直接传入，而是由输入场景几何推导出来

- `cfg.bmin`
- `cfg.bmax`
- `cfg.width`
- `cfg.height`

这些值的来源是：

- `rcCalcBounds()` 用 `101.obj` 的全部顶点算出世界坐标包围盒
- `rcCalcGridSize()` 再结合 `bmin/bmax/cs` 算出体素网格宽高

所以可以把它理解成：

- 命令行参数决定“怎么烘”
- `101.obj` 决定“烘哪一块空间、需要多大的体素网格”

#### 3.2.2 `dtNavMeshCreateParams` 里的参数又是从哪来

`buildSoloNavMesh()` 后半段还会把 Recast 结果拷到 `dtNavMeshCreateParams` 里。这里也分两类：

直接来自 `pmesh/dmesh`

- `params.verts = pmesh->verts`
- `params.vertCount = pmesh->nverts`
- `params.polys = pmesh->polys`
- `params.polyAreas = pmesh->areas`
- `params.polyFlags = pmesh->flags`
- `params.polyCount = pmesh->npolys`
- `params.nvp = pmesh->nvp`
- `params.detailMeshes = dmesh->meshes`
- `params.detailVerts = dmesh->verts`
- `params.detailVertsCount = dmesh->nverts`
- `params.detailTris = dmesh->tris`
- `params.detailTriCount = dmesh->ntris`
- `params.bmin = pmesh->bmin`
- `params.bmax = pmesh->bmax`

直接来自原始 `settings/cfg`

- `params.walkableHeight = settings.agentHeight`
- `params.walkableRadius = settings.agentRadius`
- `params.walkableClimb = settings.agentMaxClimb`
- `params.cs = cfg.cs`
- `params.ch = cfg.ch`

这里要注意一个小点：

- `cfg.walkableHeight / walkableClimb / walkableRadius` 是体素单位
- `params.walkableHeight / walkableClimb / walkableRadius` 又回到了世界单位

也就是说：

- Recast 阶段主要用体素单位做构网
- Detour 落盘阶段会保留世界单位语义

#### 3.3 Recast 烘焙流水线

参数准备好后，会按标准 Recast 流程往下跑：

1. `rcMarkWalkableTriangles`
2. `rcCreateHeightfield`
3. `rcRasterizeTriangles`
4. `rcFilterLowHangingWalkableObstacles`
5. `rcFilterLedgeSpans`
6. `rcFilterWalkableLowHeightSpans`
7. `rcBuildCompactHeightfield`
8. `rcErodeWalkableArea`
9. `rcBuildDistanceField`
10. `rcBuildRegions`
11. `rcBuildContours`
12. `rcBuildPolyMesh`
13. `rcBuildPolyMeshDetail`

可以把这段流程理解成：

- 原始三角网格
- 先体素化
- 再过滤不可走区域
- 再合并/简化区域
- 最后生成 Detour 可用的多边形网格和 detail mesh

#### 3.3.1 `pmesh` 到底是什么，它不是传进来的，而是算出来的

`pmesh` 这部分很容易误会。它不是命令行参数，也不是 `101.obj` 里直接带出来的结构，而是 Recast 烘焙流水线中的一个中间结果。

它的生成过程是：

1. `101.obj` 先被解析成原始三角网格 `verts + tris`
2. 三角网格被体素化成 `solid`（`rcHeightfield`）
3. `solid` 过滤、压缩后得到 `chf`（`rcCompactHeightfield`）
4. `chf` 提取轮廓后得到 `cset`（`rcContourSet`）
5. `rcBuildPolyMesh(&ctx, *cset, cfg.maxVertsPerPoly, *pmesh)` 才真正生成 `pmesh`

所以关系是：

```text
OBJ -> solid -> chf -> cset -> pmesh -> dmesh -> dtNavMeshCreateParams
```

也就是说，`pmesh` 是“轮廓经过多边形化之后的导航多边形网格”。

#### 3.3.2 `pmesh` 里都有什么

`rcPolyMesh` 的核心字段包括：

- `verts`
  - 多边形网格顶点，坐标是 Recast 网格里的顶点数据
- `polys`
  - 每个 polygon 使用哪些顶点，以及邻接信息
- `regs`
  - polygon 属于哪个 region
- `flags`
  - 用户自定义 flags
- `areas`
  - polygon 的 area id
- `nverts`
  - 顶点数
- `npolys`
  - polygon 数量
- `nvp`
  - 每个 polygon 允许的最大顶点数
- `bmin / bmax`
  - 这个 poly mesh 的世界坐标包围盒
- `cs / ch`
  - 体素尺寸

在当前工程里：

- `pmesh->areas` 是 Recast 先算出来的
- 然后项目代码会把 `RC_WALKABLE_AREA` 改成 `0`
- 再把 `area == 0` 的 polygon 统一打上 `flag = 1`

对应代码就在 `buildSoloNavMesh()` 里 `for (int i = 0; i < pmesh->npolys; ++i)` 这一段。

#### 3.3.3 `pmesh` 里面哪些值来自上游阶段

`pmesh` 本身不是“凭空生成”的，它会继承前面几个阶段的结果：

- `pmesh->bmin / bmax / cs / ch / borderSize / maxEdgeError`
  - 在 `rcBuildPolyMesh()` 里直接从 `cset` 拷贝
- `pmesh->verts / polys / regs / areas / nverts / npolys`
  - 在 `rcBuildPolyMesh()` 内部根据 `cset.conts` 的轮廓数据做三角化、合并、邻接构建后计算出来
- `pmesh->flags`
  - 先分配内存，再由当前工程在 `buildSoloNavMesh()` 里按 area 二次填写

所以如果你在调试里想看“`pmesh` 是怎么来的”，最值得看的断点顺序是：

1. `rcBuildContours(...)`
2. `rcBuildPolyMesh(...)`
3. `rcBuildPolyMeshDetail(...)`
4. `buildSoloNavMesh()` 里给 `pmesh->areas/flags` 二次赋值的那段循环

#### 3.3.4 `dmesh` 和 `pmesh` 的关系

`dmesh` 不是用来替代 `pmesh` 的，而是给 `pmesh` 补充更细的高度细节。

生成方式是：

- `rcBuildPolyMeshDetail(&ctx, *pmesh, *chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *dmesh)`

它依赖两个输入：

- `pmesh`
  - 提供多边形边界
- `chf`
  - 提供高度场细节

所以：

- `pmesh` 决定导航多边形拓扑
- `dmesh` 决定更细的高度三角形细节

#### 3.4 转成 Detour navmesh

当 `rcPolyMesh` 和 `rcPolyMeshDetail` 都建好后，代码会继续做两步：

- 把 `RC_WALKABLE_AREA` 改写成项目使用的 `area/flags`
- 填充 `dtNavMeshCreateParams`
- 调用 `dtCreateNavMeshData()` 生成 tile 二进制
- 调用 `dtNavMesh::init()` 初始化 Detour navmesh

当前这条链路构建的是单 tile navmesh，所以你在 `101` 的样例里会看到：

- `tileCount = 1`

### 4. 为什么输出的不是裸 Detour 数据，而是项目自定义 `.navmesh`

烘焙完成后，程序不会直接把 `dtCreateNavMeshData()` 的裸 tile blob 原样落盘，而是会再包一层项目自己的外壳：

- `NavMeshSetHeader`
- `NavMeshTileHeader`
- `Tile Data`

这层封装就是当前工程真正加载的 `.navmesh` 文件格式。

对应关系是：

- `saveWrappedNavMesh()` 负责写二进制 `.navmesh`
- `exportTopDownSvg()` 负责可选的俯视图 `svg`

所以一次烘焙通常会得到两个产物：

- `xxx.navmesh`
  - 运行时真正加载的文件
- `xxx.svg`
  - 只用于肉眼检查，不参与寻路

### 5. 生成出来以后，怎么继续验证

推荐的后续检查顺序是：

#### 5.1 看文件头和 tile 摘要

```bash
./build/navmesh_dump output_101_navmesh/101_default.navmesh
```

这个工具主要确认：

- 文件头版本
- `tileCount`
- `polyCount`
- `vertCount`
- `detailTriCount`
- `bmin/bmax`
- `dataSize` 是否和头信息一致

#### 5.2 导出成结构化 JSON

```bash
./build/navmesh_export_json \
  output_101_navmesh/101_default.navmesh \
  output_101_navmesh/101_default_polygons.json
```

导出来的内容会包含：

- tile 数量
- walkable polygon 数量
- 每个 polygon 的 `area`
- `flags`
- `centroid`
- `vertices`

#### 5.3 导出成 OBJ 做 3D 对比

```bash
./build/navmesh_export_obj \
  output_101_navmesh/101_default.navmesh \
  output_101_navmesh/101_default.obj
```

这个 `obj` 不是原始场景 `101.obj`，而是“从 navmesh 反导出来的 detail triangles”，适合拿去做 3D 结果对比。

#### 5.4 跑最小寻路验证

```bash
./build/navmesh_query_demo output_101_navmesh/101_default.navmesh
```

它会直接对刚生成的 navmesh 做：

- `findPath`
- `findStraightPath`
- `moveAlongSurface`
- `raycast`
- `findDistanceToWall`

### 6. 如果要让运行时按名字加载，文件应该放哪

运行时按名字加载时，走的是 `Navigation::loadNavigation(name)` -> `NavMeshHandle::create(name)` 这条链路。对本仓库本地调试来说，`NavMeshHandle` 会优先尝试：

- `res/<name>.navmesh`
- `D:/e996_navmesh/res/<name>.navmesh`
- `/mnt/d/e996_navmesh/res/<name>.navmesh`

所以如果你希望运行：

```bash
./build/navigation_load_by_name_demo 101_nav
```

那么它实际会去找：

```text
res/101_nav.navmesh
```

也就是说：

- `output_101_navmesh/*.navmesh` 更适合做烘焙产物和比对样例
- `res/<name>.navmesh` 才是本仓库里“按名字加载”的默认测试入口

### 7. 当前仓库里这些 `101` 相关产物分别是什么

这一段很重要，因为目录里的名字带有历史痕迹，不一定和“当前命令默认值”一一对应。

- `101.obj`
  - 原始输入场景
- `output_101_navmesh/101.obj`
  - 上面这份输入文件的拷贝
- `res/101_nav.navmesh`
  - 当前仓库里给 `navigation_load_by_name_demo 101_nav` 用的运行时样例
  - 我本地 `2026-04-23` 用 `navmesh_dump` 查看时，摘要是：
    - `tileCount=1`
    - `polyCount=3960`
    - `vertCount=7949`
- `output_101_navmesh/101_client_like.navmesh`
  - 一个历史对比样例
  - 我本地 `2026-04-23` 查看时，摘要是：
    - `tileCount=1`
    - `polyCount=7981`
    - `vertCount=13473`
  - 这个名字表达的是“某组客户端侧风格参数”，不要默认把它等同于“当前代码默认输出”
- `output_101_navmesh/101_web_like.navmesh`
  - 另一个历史对比样例，粒度更细
  - 我本地 `2026-04-23` 查看时，摘要是：
    - `tileCount=1`
    - `polyCount=7510`
    - `vertCount=15000`
  - 这个名字也不要机械理解成“只要跑 README 命令就一定字节级复现”
- `output_101_navmesh/101_polygons.json`
  - 历史导出文件
  - 当前文件内部的 `source_navmesh` 仍然写着 `101_formal.navmesh`
  - 说明这份 `json` 是较早一次导出的结果，不应直接当作“当前命令最新产物”

### 8. 推荐你后面继续沿用的实际工作流

如果后面你要继续围绕 `101.obj` 调参数，我建议固定用下面这条链路，最不容易混淆：

1. 以 `101.obj` 为唯一输入源
2. 每跑一套参数就输出一个新名字，例如 `101_default.navmesh`、`101_fine.navmesh`
3. 同时导出对应的 `svg`
4. 用 `navmesh_dump` 看头信息
5. 用 `navmesh_export_json` / `navmesh_export_obj` 做结构和图形对比
6. 只有在需要走运行时“按名字加载”时，才把目标文件放到 `res/<name>.navmesh`

这样就能把下面几件事彻底分开：

- 原始输入 `obj`
- 当前一次烘焙产物
- 历史对比样例
- 运行时加载用样例
