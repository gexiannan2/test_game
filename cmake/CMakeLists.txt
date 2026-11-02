message(STATUS ">>> ↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓")
message(STATUS ">>> 顶层 CMakeLists 开始......")

cmake_minimum_required(VERSION 3.28.0)

# ---------- 强制指定编译器（可选） ----------
set(CMAKE_C_COMPILER   gcc   CACHE STRING "C compiler" FORCE)
set(CMAKE_CXX_COMPILER g++   CACHE STRING "C++ compiler" FORCE)

# ---------- 启用 ccache（必须在 project() 之前设置 launcher） ----------
option(ENABLE_CCACHE "Enable ccache if available" ON)
if(ENABLE_CCACHE)
    find_program(CCACHE_PROGRAM ccache)
    if(CCACHE_PROGRAM)
        message(STATUS "ccache enabled: ${CCACHE_PROGRAM}")
        set(CMAKE_C_COMPILER_LAUNCHER   ${CCACHE_PROGRAM})
        set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
    else()
        message(STATUS "ccache requested but not found")
    endif()
endif()

project(projs LANGUAGES C CXX)

# ---------- C++ 标准 ----------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------- Sanitizers（ASan / UBSan） ----------
# 开启方式: cmake -DE996_ENABLE_ASAN=ON -DE996_ENABLE_UBSAN=ON ...
# 启用后, 所有经 src/common/CMakeLists.txt 构建的服务目标会自动挂载 sanitizer,
# 可抓取野指针/空指针/UAF/double free 等内存问题现场崩溃日志。
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(Sanitizers)

if((E996_ENABLE_ASAN OR E996_ENABLE_UBSAN) AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

e996_configure_sanitizers()

# ---------- 生成 compile_commands.json（供 IDE/工具使用） ----------
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------- 构建目标选择 ----------
if(NOT DEFINED BUILD_TARGET)
    set(BUILD_TARGET "all" CACHE STRING "build target: all or single module")
endif()

# ---------- 源码根目录 ----------
set(E996_SRC_ROOT "${CMAKE_SOURCE_DIR}/src")
set(GAME2D_DIR "${E996_SRC_ROOT}/svc_game/2d")
set(GAME3D_DIR "${E996_SRC_ROOT}/svc_game/3d")

# ---------- 1. 加载 e996_cgame ----------
if(EXISTS "${GAME2D_DIR}/CMakeLists.txt")
    set(CGAME_DIR "${E996_SRC_ROOT}/e996_mmo_c/src/e996_cgame")
else()
    set(CGAME_DIR "${E996_SRC_ROOT}/e996_mmo_c/src/e996_cgame3d")
endif()
if(EXISTS "${CGAME_DIR}/CMakeLists.txt")
    add_subdirectory("${CGAME_DIR}")
endif()

# ---------- 2. 加载游戏模块（2D / 3D 二选一） ----------
if(EXISTS "${GAME2D_DIR}/CMakeLists.txt")
    if (BUILD_TARGET STREQUAL "all" OR BUILD_TARGET STREQUAL "svc_game")
        add_subdirectory("${GAME2D_DIR}")
    endif() 
elseif(EXISTS "${GAME3D_DIR}/CMakeLists.txt")
    if (BUILD_TARGET STREQUAL "all" OR BUILD_TARGET STREQUAL "svc_game")
        add_subdirectory("${GAME3D_DIR}")
    endif()
else()
    message(FATAL_ERROR ">>> 未找到 svc_game/2d 或 svc_game/3d，构建终止！")
endif()

# ---------- 3. 加载其他服务模块 ----------
set(TARGETS
    svc_center
    svc_gate
    svc_login
    svc_log
    svc_cache
    svc_guard
    robot
    robotc
)

foreach(tgt ${TARGETS})
    # 如果指定了 BUILD_TARGET 且不是 all，只编译指定的模块
    if(NOT (BUILD_TARGET STREQUAL "all" OR BUILD_TARGET STREQUAL "${tgt}"))
        continue()
    endif()

    set(TGT_DIR "${E996_SRC_ROOT}/${tgt}")
    if(EXISTS "${TGT_DIR}/CMakeLists.txt")
        add_subdirectory("${TGT_DIR}")
        message(STATUS ">>> 已加载: ${tgt}")
    else()
        message(FATAL_ERROR ">>> 目标目录不存在: ${TGT_DIR}")
    endif()
endforeach()

# ---------- 4. 跨目录依赖 ----------
# e996_cgame 的链接由各游戏模块的 CMakeLists.txt 自行处理：
#   - svc_game3d/CMakeLists.txt: add_subdirectory(e996_cgame) + target_link_libraries(... PRIVATE e996_cgame)
#   - svc_game2d/CMakeLists.txt (如有): 同上
# 此处不再做重复链接，避免重复定义。

# ---------- 5. 打印最终构建信息 ----------
message(STATUS ">>> 构建目标: ${BUILD_TARGET}")
message(STATUS ">>> 源码根目录: ${E996_SRC_ROOT}")
message(STATUS ">>> 顶层 CMakeLists 结束!!!!!!")
message(STATUS ">>> ↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑")