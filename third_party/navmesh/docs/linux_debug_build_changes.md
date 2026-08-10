# Linux Debug 远程构建配置改动总结

## 背景

原始项目只能在 Linux 本地通过命令行 `cmake` 构建。需要在 **Visual Studio 2026** 中添加 **Linux Debug** 构建选项，通过 **WSL Ubuntu-24.04** 远程编译生成 `libnavmesh.so`。

## 核心问题

VS 2026 远程构建时会用 rsync 把项目目录复制到 WSL 的 `/root/.vs/lib_navmesh/`，但项目外部的 `common/`、`release/` 等依赖目录不会被复制，导致：

1. `include(../../../../common/CMakeLists.txt)` 找不到文件
2. 公共 CMakeLists 的 `list(TRANSFORM ... PREPEND)` 会把绝对路径错误拼接成 `/root/.vs/lib_navmesh/mnt/f/...`
3. 编译时找不到 `e996_capi.h`、`lua.hpp`、`singleton.h` 等头文件
4. CMake 编译器检测文件被错误编入目标，导致链接时符号重复定义

## 改动文件清单

| 文件 | 类型 | 说明 |
|------|------|------|
| `lib_navmesh/CMakePresets.json` | 新增 | CMake 预设配置，定义 linux-debug 和 x64-debug |
| `lib_navmesh/CMakeLists.txt` | 修改 | 路径变量化，支持远程构建 |
| `lib_navmesh/src/3d_nav.cpp` | 修改 | 删除未使用的 `#include "client_map.pb.h"` |
| `common/CMakeLists.txt` | 修改 | 修复绝对路径拼接 + 排除编译器检测文件 |

---

## 详细改动

### 1. 新增 `lib_navmesh/CMakePresets.json`

**作用**：定义两个构建预设，让 VS 工具栏下拉框显示 `Linux Debug` 和 `x64 Debug` 选项。

```json
{
  "version": 5,
  "configurePresets": [
    {
      "name": "linux-debug",
      "displayName": "Linux Debug",
      "description": "WSL Ubuntu-24.04 远程调试构建",
      "generator": "Unix Makefiles",
      "binaryDir": "${sourceDir}/out/build/${presetName}",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Debug",
        "CMAKE_INSTALL_PREFIX": "${sourceDir}/out/install/${presetName}",
        "COMMON_CMAKE_DIR": "/mnt/f/e996_builder-new/e996_mmo/src/common",
        "PROJECT_ROOT_DIR": "/mnt/f/e996_builder-new/e996_mmo/src/svc_game/3d/lib_navmesh"
      }
    },
    {
      "name": "x64-debug",
      "displayName": "x64 Debug",
      "description": "本地 Windows 调试构建",
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/out/build/${presetName}",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Debug",
        "CMAKE_INSTALL_PREFIX": "${sourceDir}/out/install/${presetName}"
      },
      "architecture": { "value": "x64", "strategy": "set" }
    }
  ],
  "buildPresets": [...]
}
```

**关键变量**：
- `COMMON_CMAKE_DIR`：WSL 中公共 CMakeLists 的路径
- `PROJECT_ROOT_DIR`：WSL 中项目根目录路径（用于计算其他相对路径）

---

### 2. 修改 `lib_navmesh/CMakeLists.txt`

#### 2.1 新增路径基配置（第12-30行）

在 `LIB_DIRS` / `INC_DIRS` 之前定义路径变量，支持本地和远程两种场景：

```cmake
# ┌──────────── 路径基配置（必须在 LIB_DIRS / INC_DIRS 之前）────────────
if(NOT COMMON_CMAKE_DIR)
    set(COMMON_CMAKE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../common")
endif()
if(NOT PROJECT_ROOT_DIR)
    set(PROJECT_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
# common 在 src 下，SRC_ROOT_DIR = 往上 3 级 = e996_mmo/src
get_filename_component(SRC_ROOT_DIR    "${PROJECT_ROOT_DIR}/../../.."         ABSOLUTE)
# release 与 e996_mmo 同级，RELEASE_ROOT_DIR = e996_builder-new/release
get_filename_component(RELEASE_ROOT_DIR "${PROJECT_ROOT_DIR}/../../../../../release" ABSOLUTE)
```

**路径层级关系**：
```
lib_navmesh (0)
  └── 3d (1)
       └── svc_game (2)
            └── src (3)        ← SRC_ROOT_DIR
                 └── e996_mmo (4)
                      └── e996_builder-new (5)
                           └── release (5/release)  ← RELEASE_ROOT_DIR
```

#### 2.2 替换所有 `../../../../` 相对路径为变量

| 原路径 | 新路径 |
|--------|--------|
| `"../../../../common/protobuf"` | `"${SRC_ROOT_DIR}/common/protobuf"` |
| `"../../../../common/lua"` | `"${SRC_ROOT_DIR}/common/lua"` |
| `"../../../../common/e996_capi"` | `"${SRC_ROOT_DIR}/common/e996_capi"` |
| `"../../../../common/e996_lua"` | `"${SRC_ROOT_DIR}/common/e996_lua"` |
| `"../../../../common/protobuf/include"` | `"${SRC_ROOT_DIR}/common/protobuf/include"` |
| `"../../../../../release/e996_lua_game_frame/Protos/c++"` | `"${RELEASE_ROOT_DIR}/e996_lua_game_frame/Protos/c++"` |
| `"../../../../../release/e996_lua_ver_frame/Protos/c++"` | `"${RELEASE_ROOT_DIR}/e996_lua_ver_frame/Protos/c++"` |
| `"${CMAKE_CURRENT_SOURCE_DIR}/../../../../../release/bin"` | `"${RELEASE_ROOT_DIR}/bin"` |

#### 2.3 修改公共 CMakeLists 引用

```cmake
# 修改前
include(../../../../common/CMakeLists.txt)

# 修改后
include(${COMMON_CMAKE_DIR}/CMakeLists.txt)
```

---

### 3. 修改 `common/CMakeLists.txt`

#### 3.1 修复绝对路径被错误拼接（第12-40行）

**问题**：原代码用 `list(TRANSFORM ... PREPEND)` 给所有路径加前缀，但绝对路径（如 `/mnt/f/...`）会被错误拼接成 `/root/.vs/lib_navmesh/mnt/f/...`。

```cmake
# 修改前
list(TRANSFORM LINK_DIRS PREPEND  "${CMAKE_CURRENT_SOURCE_DIR}/")
list(TRANSFORM INC_DIRS PREPEND   "${CMAKE_CURRENT_SOURCE_DIR}/")
list(TRANSFORM SRC_FILES PREPEND  "${CMAKE_CURRENT_SOURCE_DIR}/")

# 修改后：仅对相对路径添加前缀，绝对路径直接保留
foreach(d IN LISTS LINK_DIRS)
    if(IS_ABSOLUTE "${d}")
        list(APPEND _tmp "${d}")
    else()
        list(APPEND _tmp "${CMAKE_CURRENT_SOURCE_DIR}/${d}")
    endif()
endforeach()
set(LINK_DIRS ${_tmp})
# INC_DIRS、SRC_FILES 同样处理
```

#### 3.2 修复绝对目录生成（第79-110行）

同样的问题，`LIB_DIRS`、`SRC_DIRS`、`WHOLE_ARCHIVE_LIBS` 的 PREPEND 也需要修复：

```cmake
# 修改前
list(TRANSFORM LIB_DIRS PREPEND  "${CMAKE_CURRENT_SOURCE_DIR}/")
list(TRANSFORM SRC_DIRS PREPEND  "${CMAKE_CURRENT_SOURCE_DIR}/")
list(TRANSFORM WHOLE_ARCHIVE_LIBS PREPEND  "${CMAKE_CURRENT_SOURCE_DIR}/")

# 修改后：同样用 foreach + IS_ABSOLUTE 判断
```

#### 3.3 排除编译器检测文件（第209-215行）

**问题**：公共 CMakeLists 把项目根目录加入 `SRC_DIRS`，递归扫描 `.c/.cpp` 时把 `out/build/.../CMakeFiles/` 下的编译器检测文件也扫进来了，导致 `main`、`info_compiler` 等符号重复定义，链接失败。

```cmake
# 修改前
list(FILTER FILES EXCLUDE REGEX "\\.(BASE|LOCAL|REMOTE)\\.")
list(APPEND SRC_FILES ${FILES})

# 修改后：增加构建目录过滤
list(FILTER FILES EXCLUDE REGEX "\\.(BASE|LOCAL|REMOTE)\\.")
# 排除构建输出目录中的编译器检测文件，避免被错误编入目标
# 注意: 不能排除 /.vs/，因为 VS 远程构建会把项目复制到 /root/.vs/<project>/
list(FILTER FILES EXCLUDE REGEX "/out/build/")
list(FILTER FILES EXCLUDE REGEX "/CMakeFiles/3\\.[0-9]")
list(APPEND SRC_FILES ${FILES})
```

**踩坑记录**：
- 初次尝试用 `EXCLUDE REGEX "/\\.vs/"` 排除，结果把**所有**源文件都排除了，因为 VS 远程项目就在 `/root/.vs/lib_navmesh/` 下，所有文件路径都含 `/.vs/`
- 正确做法：只排除 `/out/build/` 和 `/CMakeFiles/3.x`（CMake 版本检测目录）

---

### 4. 修改 `lib_navmesh/src/3d_nav.cpp`

**问题**：`#include "client_map.pb.h"` 引入了不存在的协议头文件。

**排查结果**：在 `3d_nav.cpp`（990行）中没有任何地方使用 `client_map.pb.h` 中的类型或函数，是无用的头文件引入。

```cpp
// 修改前
#include "3d_nav.h"
#include "client_map.pb.h"    // ← 删除
#include "e996_capi_bf.h"

// 修改后
#include "3d_nav.h"
#include "e996_capi_bf.h"
```

---

## 使用方法

### 在 Visual Studio 2026 中构建

1. 打开 `lib_navmesh` 文件夹
2. 工具栏下拉框选择 **Linux Debug**
3. `CMake → 配置缓存`（首次会复制文件到 WSL）
4. `CMake → 全部生成`

### 构建产物

```
本地 Windows 缓存:  F:\...\lib_navmesh\out\build\linux-debug\
WSL 远程构建目录:   /root/.vs/lib_navmesh/out/build/linux-debug/
libnavmesh.so:      /root/.vs/lib_navmesh/lib/libnavmesh.so
                    → 自动拷贝到 /mnt/f/e996_builder-new/release/bin/libnavmesh.so
bench_find_path:    /mnt/f/e996_builder-new/release/bin/bench_find_path
bench_find_path_by_navigation: /mnt/f/e996_builder-new/release/bin/bench_find_path_by_navigation
```

### 预期 CMake 配置日志

```
[CMake] -- >>> COMMON_CMAKE_DIR  : /mnt/f/e996_builder-new/e996_mmo/src/common
[CMake] -- >>> PROJECT_ROOT_DIR  : /mnt/f/e996_builder-new/e996_mmo/src/svc_game/3d/lib_navmesh
[CMake] -- >>> SRC_ROOT_DIR      : /mnt/f/e996_builder-new/e996_mmo/src
[CMake] -- >>> RELEASE_ROOT_DIR  : /mnt/f/e996_builder-new/release
[CMake] -- >>> LIB_DIRS: /mnt/f/e996_builder-new/e996_mmo/src/common/protobuf;/mnt/f/e996_builder-new/e996_mmo/src/common/lua
```

---

## 注意事项

1. **路径硬编码问题**：`CMakePresets.json` 中的 `COMMON_CMAKE_DIR` 和 `PROJECT_ROOT_DIR` 硬编码了 `/mnt/f/...` 路径。如果项目迁移到其他盘符，需要同步修改。

2. **公共 CMakeLists 影响范围**：`common/CMakeLists.txt` 是公共文件，被多个项目 include。修改后其他项目也会受益（绝对路径不再被错误拼接），但建议在其他项目验证一下。

3. **清理缓存**：修改 CMakeLists 或 CMakePresets 后，需要 `CMake → 清除缓存` 重新配置，否则可能使用旧缓存。同时需要清除 WSL 远程缓存：`wsl -d Ubuntu-24.04 -- rm -rf /root/.vs/lib_navmesh`

4. **WSL 文件同步**：VS 通过 rsync 同步项目文件到 WSL。如果发现 WSL 中文件缺失（如 `depend/common` 目录不存在），清除 `/root/.vs/lib_navmesh` 后重新配置 CMake 即可触发完整同步。

5. **构建性能**：通过 `/mnt/f/` 访问 Windows 文件系统 IO 较慢，首次配置和链接会比较慢，后续增量构建会快很多。
