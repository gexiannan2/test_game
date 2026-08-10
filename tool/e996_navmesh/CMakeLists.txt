cmake_minimum_required(VERSION 3.28)
project(navigation)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()
if(CMAKE_BUILD_TYPE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()
if(NOT MSVC)
    if(NOT CMAKE_C_FLAGS_DEBUG MATCHES "(^| )-O[0-3s]($| )")
        set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -O0" CACHE STRING "C debug flags" FORCE)
    endif()
    if(NOT CMAKE_C_FLAGS_DEBUG MATCHES "(^| )-g($| )")
        set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -g" CACHE STRING "C debug flags" FORCE)
    endif()
    if(NOT CMAKE_CXX_FLAGS_DEBUG MATCHES "(^| )-O[0-3s]($| )")
        set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -O0" CACHE STRING "CXX debug flags" FORCE)
    endif()
    if(NOT CMAKE_CXX_FLAGS_DEBUG MATCHES "(^| )-g($| )")
        set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g" CACHE STRING "CXX debug flags" FORCE)
    endif()
endif()
# 构建产物留在构建目录，避免编译时覆盖仓库内文件。
set(NAVIGATION_LIB_OUTPUT_DIR "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${NAVIGATION_LIB_OUTPUT_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${NAVIGATION_LIB_OUTPUT_DIR}")
if(NOT MSVC)
    add_definitions(-g)
endif()

set(NAVIGATION_ROOT_SRCS
    DetourAlloc.cpp
    DetourAssert.cpp
    DetourCommon.cpp
    DetourCrowd.cpp
    DetourLocalBoundary.cpp
    DetourNavMesh.cpp
    DetourNavMeshBuilder.cpp
    DetourNavMeshQuery.cpp
    DetourNode.cpp
    DetourObstacleAvoidance.cpp
    DetourPathCorridor.cpp
    DetourPathQueue.cpp
    DetourProximityGrid.cpp
    DetourTileCache.cpp
    DetourTileCacheBuilder.cpp
    navmesh_baker.cpp
    navigation.cpp
    navigation_handle.cpp
    navigation_mesh_handle.cpp
    Recast.cpp
    RecastAlloc.cpp
    RecastArea.cpp
    RecastAssert.cpp
    RecastContour.cpp
    RecastFilter.cpp
    RecastLayers.cpp
    RecastMesh.cpp
    RecastMeshDetail.cpp
    RecastRasterization.cpp
    RecastRegion.cpp
)

# C API 头文件属于可选兄弟工程，缺失时不影响烘焙与查询工具。
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../e996_capi/e996_capi_nav.h")
    list(APPEND NAVIGATION_ROOT_SRCS e996_capi_nav.cpp)
endif()

set(NAVIGATION_DEP_SRCS
    depend/common/common.cpp
    depend/common/format.cpp
    depend/common/kbeversion.cpp
    depend/common/memorystream.cpp
    depend/common/rsa.cpp
    depend/common/sha1.cpp
    depend/common/strutil.cpp
    depend/common/timer.cpp
    depend/common/timestamp.cpp
    depend/math/lmath.cpp
    depend/resmgr/resmgr.cpp
    depend/resmgr/resourceobject.cpp
)

set(NAVIGATION_INCLUDE_DIRS
    .
    ..
    depend
    depend/g3dlite
    ../e996_capi
)

IF(WIN32)
    add_library(navigation STATIC ${NAVIGATION_ROOT_SRCS} ${NAVIGATION_DEP_SRCS})
ELSE()
    add_library(navigation SHARED ${NAVIGATION_ROOT_SRCS} ${NAVIGATION_DEP_SRCS})
ENDIF()

target_include_directories(navigation PRIVATE ${NAVIGATION_INCLUDE_DIRS})
if(MSVC)
    target_compile_definitions(navigation PRIVATE UNICODE _UNICODE)
endif()

add_executable(navmesh_dump tools/navmesh_dump.cpp)
target_include_directories(navmesh_dump PRIVATE .)

add_executable(navmesh_bake_from_obj tools/navmesh_bake_from_obj.cpp)
target_include_directories(navmesh_bake_from_obj PRIVATE .)
target_link_libraries(navmesh_bake_from_obj PRIVATE navigation)

add_executable(navmesh_query_demo tools/navmesh_query_demo.cpp)
target_include_directories(navmesh_query_demo PRIVATE .)
target_link_libraries(navmesh_query_demo PRIVATE navigation)

add_executable(navmesh_path_compare tools/navmesh_path_compare.cpp)
target_include_directories(navmesh_path_compare PRIVATE .)
target_link_libraries(navmesh_path_compare PRIVATE navigation)

# 这两个历史示例并非所有源码包都包含，存在时才创建目标。
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/navmesh_triangle_path_demo.cpp")
    add_executable(navmesh_triangle_path_demo tools/navmesh_triangle_path_demo.cpp)
    target_include_directories(navmesh_triangle_path_demo PRIVATE .)
    target_link_libraries(navmesh_triangle_path_demo PRIVATE navigation)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/navmesh_smooth_path_demo.cpp")
    add_executable(navmesh_smooth_path_demo tools/navmesh_smooth_path_demo.cpp)
    target_include_directories(navmesh_smooth_path_demo PRIVATE .)
    target_link_libraries(navmesh_smooth_path_demo PRIVATE navigation)
endif()

add_executable(navmesh_export_json tools/navmesh_export_json.cpp)
target_include_directories(navmesh_export_json PRIVATE .)
target_link_libraries(navmesh_export_json PRIVATE navigation)

add_executable(navmesh_export_obj tools/navmesh_export_obj.cpp)
target_include_directories(navmesh_export_obj PRIVATE .)
target_link_libraries(navmesh_export_obj PRIVATE navigation)

add_executable(navigation_load_by_name_demo test/navigation_load_by_name_demo.cpp)
target_include_directories(navigation_load_by_name_demo PRIVATE ${NAVIGATION_INCLUDE_DIRS})
target_link_libraries(navigation_load_by_name_demo PRIVATE navigation)

