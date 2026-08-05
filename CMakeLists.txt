# svc_game_3d — 独立构建工程（WSL Linux 直接 cmake + make）
#
# 目录结构:
#   <project_root>/
#   ├── CMakeLists.txt       ← 本文件
#   ├── src/                  源码 (client/server/protocol/common)
#   ├── third_party/          zrpc + joltphysics
#   ├── deps/protobuf/         protobuf 头文件
#   ├── protobuf/             proto 工具 + 原始 .proto + 生成代码
#   │   └── protos/c++/       ← proto 生成 .pb.cc/.pb.h/.auto.cpp (CMake 直接引用)
#   ├── lib/                  libprotobufd.so*
#   └── bin/                  编译产物输出目录
#
# WSL 构建命令 (in-source build):
#   cd <project_root>
#   cmake .                       # 生成 Makefile (首次或改 CMakeLists 后)
#   make -j$(nproc)               # 编译全部
#   make svc_game_3d_server       # 只编译 server
#   make clean                    # 清理 .o (保留 Makefile)
#   # 彻底清理: rm -f Makefile cmake_install.cmake CMakeCache.txt && rm -rf CMakeFiles
#   # 产物在 bin/ 下

cmake_minimum_required(VERSION 3.21.0)


project(svc_game_3d LANGUAGES C CXX)

# ---------- C++ 标准 ----------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

message(STATUS ">>> Build Type: ${CMAKE_BUILD_TYPE}")

# ---------- 路径定义 ----------
set(SRC_DIR          "${CMAKE_SOURCE_DIR}/src")
set(ZRPC_DIR         "${CMAKE_SOURCE_DIR}/third_party/zrpc")
set(JOLT_DIR         "${CMAKE_SOURCE_DIR}/third_party/joltphysics")
set(PROTO_GEN_DIR    "${CMAKE_SOURCE_DIR}/protobuf/protos/c++")
set(PROTO_LUA_DIR    "${CMAKE_SOURCE_DIR}/protobuf/protos/lua")

# ---- 编译时自动从 Msg_Cli_protobuf.lua 生成 proto_name_lookup.h ----
set(PROTO_LUA_FILE  "${CMAKE_SOURCE_DIR}/protobuf/protos/Msg_Cli_protobuf.lua")
set(PROTO_LOOKUP_H  "${PROTO_GEN_DIR}/proto_name_lookup.h")
add_custom_command(
    OUTPUT  "${PROTO_LOOKUP_H}"
    COMMAND ${CMAKE_COMMAND}
            "-DPROTO_LUA_FILE=${PROTO_LUA_FILE}"
            "-DPROTO_LOOKUP_H=${PROTO_LOOKUP_H}"
            "-P" "${CMAKE_SOURCE_DIR}/cmake/gen_proto_lookup.cmake"
    DEPENDS "${PROTO_LUA_FILE}"
    COMMENT ">>> Generating proto_name_lookup.h from Msg_Cli_protobuf.lua"
    VERBATIM
)
add_custom_target(gen_proto_lookup DEPENDS "${PROTO_LOOKUP_H}")
set(PROTOBUF_ROOT    "${CMAKE_SOURCE_DIR}/deps/protobuf")
set(OUT_DIR          "${CMAKE_SOURCE_DIR}/bin")
set(LIB_DIR          "${CMAKE_SOURCE_DIR}/lib")

# ---------- 公共头文件目录 ----------
set(INC_DIRS
    "${SRC_DIR}"
    "${SRC_DIR}/protocol"
    "${SRC_DIR}/common"
    "${SRC_DIR}/server"
    "${SRC_DIR}/server/ecs"
    "${SRC_DIR}/server/ecs/component"
    "${SRC_DIR}/server/ecs/component_base"
    "${SRC_DIR}/server/ecs/components"
    "${SRC_DIR}/server/ecs/entity"
    "${SRC_DIR}/server/handlers"
    "${SRC_DIR}/server/session"
    "${CMAKE_SOURCE_DIR}/third_party"
    "${JOLT_DIR}/include"
    "${PROTO_GEN_DIR}"
    "${PROTO_LUA_DIR}"
    "${PROTOBUF_ROOT}/include"
)

# ---------- Mongo 玩家异步落地（源码静态编入，必选） ----------
set(MONGO_BUILD_TESTS OFF CACHE BOOL "由游戏服构建时关闭 mongo 模块自测" FORCE)
add_subdirectory("${SRC_DIR}/server/mongo")
list(APPEND INC_DIRS "${SRC_DIR}/server/mongo/include")
message(STATUS ">>> mongo player async storage enabled (src/server/mongo)")

# ---------- protobuf 库 ----------
set(PROTOBUF_LIB "protobufd")
set(PROTOBUF_LINK_DIRS "${LIB_DIR}")

message(STATUS ">>> PROTOBUF_INCLUDE : ${PROTOBUF_ROOT}/include")
message(STATUS ">>> PROTOBUF_LINK   : ${PROTOBUF_LINK_DIRS}")


# ---------- RPATH: exe 从同级目录找 .so ----------
set(CMAKE_EXE_LINKER_FLAGS    "${CMAKE_EXE_LINKER_FLAGS} -Wl,-rpath,$ORIGIN")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-rpath,$ORIGIN")

# ---------- 拷贝 libprotobufd.so 到 bin/ (首次构建, 若文件存在) ----------
foreach(_lib libprotobufd.so libprotobufd.so.3.21.12.0 libprotobufd.so.32)
    if(EXISTS "${LIB_DIR}/${_lib}")
        file(COPY "${LIB_DIR}/${_lib}" DESTINATION "${OUT_DIR}")
        message(STATUS ">>> copy: ${LIB_DIR}/${_lib} -> ${OUT_DIR}")
    else()
        message(WARNING ">>> SKIP copy: ${LIB_DIR}/${_lib} 不存在, 请确保运行时能找到该库")
    endif()
endforeach()
unset(_lib)

# ============================================================================
# 1. zrpc OBJECT 库
# ============================================================================
file(GLOB_RECURSE ZRPC_SRC CONFIGURE_DEPENDS
    "${ZRPC_DIR}/base/*.cc"
    "${ZRPC_DIR}/net/*.cc"
    "${ZRPC_DIR}/http/*.cc"
)
list(FILTER ZRPC_SRC EXCLUDE REGEX "/rpc/")
list(FILTER ZRPC_SRC EXCLUDE REGEX "/grpc/")
list(FILTER ZRPC_SRC EXCLUDE REGEX "/example/")
list(FILTER ZRPC_SRC EXCLUDE REGEX "/tools/")
list(FILTER ZRPC_SRC EXCLUDE REGEX "/test/")

add_library(zrpc OBJECT ${ZRPC_SRC})
target_include_directories(zrpc PUBLIC ${INC_DIRS})
target_compile_options(zrpc PRIVATE -w -fpermissive)

# ============================================================================
# 2. Jolt Physics OBJECT 库
# ============================================================================
file(GLOB_RECURSE JOLT_SRC CONFIGURE_DEPENDS
    "${JOLT_DIR}/include/Jolt/*.cpp"
)
add_library(jolt_physics OBJECT ${JOLT_SRC})
target_include_directories(jolt_physics PUBLIC "${JOLT_DIR}/include")
target_compile_definitions(jolt_physics PUBLIC
    JPH_OBJECT_STREAM
    JPH_OBJECT_LAYER_BITS=16
)
target_compile_options(jolt_physics PRIVATE -w -march=native)
# -march=native 自动启用当前 CPU 支持的全部指令集:
#   SSE2/SSE3/SSSE3/SSE4.1/SSE4.2/AVX/AVX2/FMA/F16C/LZCNT/TZCNT ...
# JoltPhysics 的 Core.h 会根据编译器预定义宏 (__AVX2__ 等) 自动开启对应优化
#
# JPH_OBJECT_LAYER_BITS=16: ObjectLayer 用 uint16（紧凑），游戏服 8 个 layer 只需 3 bit
# JPH_OBJECT_STREAM: 序列化支持（BodyCreationSettings 等）
# 不定义 JPH_DISABLE_CUSTOM_ALLOCATOR: 运行时 RegisterDefaultAllocator() 注册（允许替换）
# 不定义 JPH_DOUBLE_PRECISION: 单精度 float，比 double 快 30-50%+ 省一半内存
# 不定义 JPH_CROSS_PLATFORM_DETERMINISTIC: 关闭跨平台确定性以换取性能
# 不定义 JPH_ENABLE_ASSERTS: Release 模式靠 CMake 的 NDEBUG 自动关闭 JPH_DEBUG → 断言全关

# ============================================================================
# 3. protobuf 生成代码 OBJECT 库
# ============================================================================
add_library(game_proto OBJECT
    "${SRC_DIR}/protocol/pack_codec.cpp"
    "${PROTO_GEN_DIR}/game.pb.cc"
    "${PROTO_GEN_DIR}/client_login.pb.cc"
    "${PROTO_GEN_DIR}/client_3d.pb.cc"
    "${PROTO_GEN_DIR}/client_common.pb.cc"
    "${PROTO_GEN_DIR}/client_battle.pb.cc"
)
target_include_directories(game_proto PUBLIC ${INC_DIRS})
target_compile_options(game_proto PRIVATE -w -fpermissive)

# ============================================================================
# 4. svc_game_3d_server
# ============================================================================
add_executable(svc_game_3d_server
    "${SRC_DIR}/server/main.cc"
    "${SRC_DIR}/server/game_server.cpp"
    "${SRC_DIR}/server/jolt_server.cpp"
    "${SRC_DIR}/server/ecs/entity/entity.cpp"
    "${SRC_DIR}/server/ecs/entity/player_entity.cpp"
    "${SRC_DIR}/server/handlers/connection_handler.cpp"
    "${SRC_DIR}/server/handlers/account_handler.cpp"
    "${SRC_DIR}/server/handlers/role_handler.cpp"
    "${SRC_DIR}/server/handlers/game_handler.cpp"
    "${SRC_DIR}/server/handlers/move_handler.cpp"
    "${SRC_DIR}/server/ecs/systems/map_grid.cpp"
    "${SRC_DIR}/server/ecs/systems/aoi_view_bridge.cpp"
    "${SRC_DIR}/server/ecs/systems/aoi_system.cpp"
    "${SRC_DIR}/server/ecs/systems/aoi_sector.cpp"
    "${SRC_DIR}/server/ecs/systems/world_system.cpp"
    "${SRC_DIR}/server/session/connection_system.cpp"
    "${SRC_DIR}/server/session/account_system.cpp"
    "${SRC_DIR}/server/session/role_system.cpp"
    "${SRC_DIR}/server/ecs/systems/map_system.cpp"
    "${SRC_DIR}/server/ecs/systems/map_config_system.cpp"
    "${SRC_DIR}/server/ecs/systems/player_entity_system.cpp"
    "${SRC_DIR}/server/ecs/systems/move_system.cpp"
    "${SRC_DIR}/server/ecs/systems/jolt_system.cpp"
    "${SRC_DIR}/server/ecs/components/move_component.cpp"
    "${SRC_DIR}/server/ecs/systems/player_persist_system.cpp"
    $<TARGET_OBJECTS:zrpc>
    $<TARGET_OBJECTS:game_proto>
)
target_include_directories(svc_game_3d_server PRIVATE ${INC_DIRS})
target_link_directories(svc_game_3d_server PRIVATE ${PROTOBUF_LINK_DIRS})
target_link_libraries(svc_game_3d_server PRIVATE jolt_physics ${PROTOBUF_LIB} pthread)
add_dependencies(svc_game_3d_server gen_proto_lookup)
target_link_libraries(svc_game_3d_server PRIVATE mongo)
set_target_properties(svc_game_3d_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${OUT_DIR}")
target_compile_options(svc_game_3d_server PRIVATE -fpermissive -w -march=native)

# ============================================================================
# 4b. svc_game_3d_test — 单机测试（AOI/Map，不走网络）
# ============================================================================
add_executable(svc_game_3d_test
    "${SRC_DIR}/server/tests/test_main.cc"
    "${SRC_DIR}/server/tests/aoi_system_test.cc"
    "${SRC_DIR}/server/tests/aoi_broadcast_test.cc"
    "${SRC_DIR}/server/tests/aoi_parity_test.cc"
    "${SRC_DIR}/server/tests/aoi_mass_10k_test.cc"
    "${SRC_DIR}/server/tests/integration_test.cc"
    "${SRC_DIR}/server/tests/regression_test.cc"
    "${SRC_DIR}/server/tests/stress_test.cc"
    "${SRC_DIR}/server/tests/anomaly_test.cc"
    "${SRC_DIR}/server/tests/extreme_test.cc"
    "${SRC_DIR}/server/tests/long_soak_test.cc"
    "${SRC_DIR}/server/tests/production_test.cc"
    "${SRC_DIR}/server/tests/production_fullstack_test.cc"
    "${SRC_DIR}/server/tests/recursive_scan_test.cc"
    "${SRC_DIR}/server/tests/handler_logic_test.cc"
    "${SRC_DIR}/server/tests/entity_id_test.cc"
    "${SRC_DIR}/server/tests/map_system_test.cc"
    "${SRC_DIR}/server/tests/math_test.cc"
    "${SRC_DIR}/server/tests/edge_case_test.cc"
    "${SRC_DIR}/server/tests/login_aoi_churn_test.cc"
    "${SRC_DIR}/server/ecs/entity/entity.cpp"
    "${SRC_DIR}/server/ecs/entity/player_entity.cpp"
    "${SRC_DIR}/server/ecs/systems/player_entity_system.cpp"
    "${SRC_DIR}/server/session/account_system.cpp"
    "${SRC_DIR}/server/session/role_system.cpp"
    "${SRC_DIR}/server/ecs/systems/map_grid.cpp"
    "${SRC_DIR}/server/ecs/systems/map_system.cpp"
    "${SRC_DIR}/server/ecs/systems/aoi_system.cpp"
    "${SRC_DIR}/server/ecs/systems/aoi_sector.cpp"
    "${SRC_DIR}/server/ecs/systems/world_system.cpp"
    "${SRC_DIR}/server/ecs/systems/move_system.cpp"
    "${SRC_DIR}/server/jolt_server.cpp"
    "${SRC_DIR}/server/ecs/systems/jolt_system.cpp"
    "${SRC_DIR}/server/ecs/components/move_component.cpp"
    $<TARGET_OBJECTS:game_proto>
    $<TARGET_OBJECTS:zrpc>
)
target_include_directories(svc_game_3d_test PRIVATE ${INC_DIRS})
set_target_properties(svc_game_3d_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${OUT_DIR}")
target_compile_options(svc_game_3d_test PRIVATE -fpermissive -w -march=native)
target_link_libraries(svc_game_3d_test PRIVATE jolt_physics pthread)
add_dependencies(svc_game_3d_test gen_proto_lookup)
target_link_directories(svc_game_3d_test PRIVATE ${PROTOBUF_LINK_DIRS})
target_link_libraries(svc_game_3d_test PRIVATE ${PROTOBUF_LIB})

# ============================================================================
# 4c. svc_game_3d_proto_test — AOI 协议级单机测试
#     使用真实 PlayerEntity（SerializeAppear/SerializeDirty）+ WorldSystem AOI 管线，
#     捕获桥接替代网络发送，protobuf 解码断言。覆盖进出地图/AOI广播/标脏广播。
# ============================================================================
add_executable(svc_game_3d_proto_test
    "${SRC_DIR}/server/tests/test_main.cc"
    "${SRC_DIR}/server/tests/aoi_proto_test.cc"
    "${SRC_DIR}/server/tests/aoi_stress_test.cc"
    "${SRC_DIR}/server/ecs/entity/entity.cpp"
    "${SRC_DIR}/server/ecs/entity/player_entity.cpp"
    "${SRC_DIR}/server/ecs/systems/map_grid.cpp"
    "${SRC_DIR}/server/ecs/systems/map_system.cpp"
    "${SRC_DIR}/server/ecs/systems/aoi_system.cpp"
    "${SRC_DIR}/server/ecs/systems/aoi_sector.cpp"
    "${SRC_DIR}/server/ecs/systems/world_system.cpp"
    "${SRC_DIR}/server/ecs/systems/move_system.cpp"
    "${SRC_DIR}/server/jolt_server.cpp"
    "${SRC_DIR}/server/ecs/systems/jolt_system.cpp"
    "${SRC_DIR}/server/ecs/components/move_component.cpp"
    $<TARGET_OBJECTS:zrpc>
    $<TARGET_OBJECTS:game_proto>
    $<TARGET_OBJECTS:zrpc>
)
target_include_directories(svc_game_3d_proto_test PRIVATE ${INC_DIRS})
target_link_directories(svc_game_3d_proto_test PRIVATE ${PROTOBUF_LINK_DIRS})
set_target_properties(svc_game_3d_proto_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${OUT_DIR}")
target_compile_options(svc_game_3d_proto_test PRIVATE -fpermissive -w -march=native)
target_link_libraries(svc_game_3d_proto_test PRIVATE jolt_physics ${PROTOBUF_LIB} pthread)
add_dependencies(svc_game_3d_proto_test gen_proto_lookup)

# ============================================================================
# 4d. svc_game_3d_protocol_test — 端到端协议级测试（真实网络 loopback）
#     进程内启动 GameServer + 多 TcpClient，走完整协议流程验证 AOI 广播。
# ============================================================================
add_executable(svc_game_3d_protocol_test
    "${SRC_DIR}/client/protocol_test.cc"
    "${SRC_DIR}/server/game_server.cpp"
    "${SRC_DIR}/server/jolt_server.cpp"
    "${SRC_DIR}/server/ecs/entity/entity.cpp"
    "${SRC_DIR}/server/ecs/entity/player_entity.cpp"
    "${SRC_DIR}/server/handlers/connection_handler.cpp"
    "${SRC_DIR}/server/handlers/account_handler.cpp"
    "${SRC_DIR}/server/handlers/role_handler.cpp"
    "${SRC_DIR}/server/handlers/game_handler.cpp"
    "${SRC_DIR}/server/handlers/move_handler.cpp"
    "${SRC_DIR}/server/ecs/systems/map_grid.cpp"
    "${SRC_DIR}/server/ecs/systems/aoi_view_bridge.cpp"
    "${SRC_DIR}/server/ecs/systems/aoi_system.cpp"
    "${SRC_DIR}/server/ecs/systems/aoi_sector.cpp"
    "${SRC_DIR}/server/ecs/systems/world_system.cpp"
    "${SRC_DIR}/server/session/connection_system.cpp"
    "${SRC_DIR}/server/session/account_system.cpp"
    "${SRC_DIR}/server/session/role_system.cpp"
    "${SRC_DIR}/server/ecs/systems/map_system.cpp"
    "${SRC_DIR}/server/ecs/systems/map_config_system.cpp"
    "${SRC_DIR}/server/ecs/systems/player_entity_system.cpp"
    "${SRC_DIR}/server/ecs/systems/move_system.cpp"
    "${SRC_DIR}/server/ecs/systems/jolt_system.cpp"
    "${SRC_DIR}/server/ecs/components/move_component.cpp"
    "${SRC_DIR}/server/ecs/systems/player_persist_system.cpp"
    $<TARGET_OBJECTS:zrpc>
    $<TARGET_OBJECTS:game_proto>
)
target_include_directories(svc_game_3d_protocol_test PRIVATE ${INC_DIRS})
target_link_directories(svc_game_3d_protocol_test PRIVATE ${PROTOBUF_LINK_DIRS})
set_target_properties(svc_game_3d_protocol_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${OUT_DIR}")
target_compile_options(svc_game_3d_protocol_test PRIVATE -fpermissive -w -march=native)
target_link_libraries(svc_game_3d_protocol_test PRIVATE jolt_physics ${PROTOBUF_LIB} pthread)
add_dependencies(svc_game_3d_protocol_test gen_proto_lookup)
target_link_libraries(svc_game_3d_protocol_test PRIVATE mongo)

# ============================================================================
# 4e. svc_game_3d_protocol_regression_test — Handler bug 修复回归（协议级）
# ============================================================================
add_executable(svc_game_3d_protocol_regression_test
    "${SRC_DIR}/client/protocol_regression_test.cc"
    "${SRC_DIR}/server/game_server.cpp"
    "${SRC_DIR}/server/jolt_server.cpp"
    "${SRC_DIR}/server/ecs/entity/entity.cpp"
    "${SRC_DIR}/server/ecs/entity/player_entity.cpp"
    "${SRC_DIR}/server/handlers/connection_handler.cpp"
    "${SRC_DIR}/server/handlers/account_handler.cpp"
    "${SRC_DIR}/server/handlers/role_handler.cpp"
    "${SRC_DIR}/server/handlers/game_handler.cpp"
    "${SRC_DIR}/server/handlers/move_handler.cpp"
    "${SRC_DIR}/server/ecs/systems/map_grid.cpp"
    "${SRC_DIR}/server/ecs/systems/aoi_view_bridge.cpp"
    "${SRC_DIR}/server/ecs/systems/aoi_system.cpp"
    "${SRC_DIR}/server/ecs/systems/aoi_sector.cpp"
    "${SRC_DIR}/server/ecs/systems/world_system.cpp"
    "${SRC_DIR}/server/session/connection_system.cpp"
    "${SRC_DIR}/server/session/account_system.cpp"
    "${SRC_DIR}/server/session/role_system.cpp"
    "${SRC_DIR}/server/ecs/systems/map_system.cpp"
    "${SRC_DIR}/server/ecs/systems/map_config_system.cpp"
    "${SRC_DIR}/server/ecs/systems/player_entity_system.cpp"
    "${SRC_DIR}/server/ecs/systems/move_system.cpp"
    "${SRC_DIR}/server/ecs/systems/jolt_system.cpp"
    "${SRC_DIR}/server/ecs/components/move_component.cpp"
    "${SRC_DIR}/server/ecs/systems/player_persist_system.cpp"
    $<TARGET_OBJECTS:zrpc>
    $<TARGET_OBJECTS:game_proto>
)
target_include_directories(svc_game_3d_protocol_regression_test PRIVATE ${INC_DIRS})
target_link_directories(svc_game_3d_protocol_regression_test PRIVATE ${PROTOBUF_LINK_DIRS})
set_target_properties(svc_game_3d_protocol_regression_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${OUT_DIR}")
target_compile_options(svc_game_3d_protocol_regression_test PRIVATE -fpermissive -w -march=native)
target_link_libraries(svc_game_3d_protocol_regression_test PRIVATE jolt_physics ${PROTOBUF_LIB} pthread)
add_dependencies(svc_game_3d_protocol_regression_test gen_proto_lookup)
target_link_libraries(svc_game_3d_protocol_regression_test PRIVATE mongo)

# ============================================================================
# 4f. svc_game_3d_protocol_mass_test — 大规模网络协议 E2E（登录/进离图/脏同步/AOI）
# ============================================================================
add_executable(svc_game_3d_protocol_mass_test
    "${SRC_DIR}/client/protocol_mass_test.cc"
    "${SRC_DIR}/server/game_server.cpp"
    "${SRC_DIR}/server/jolt_server.cpp"
    "${SRC_DIR}/server/ecs/entity/entity.cpp"
    "${SRC_DIR}/server/ecs/entity/player_entity.cpp"
    "${SRC_DIR}/server/handlers/connection_handler.cpp"
    "${SRC_DIR}/server/handlers/account_handler.cpp"
    "${SRC_DIR}/server/handlers/role_handler.cpp"
    "${SRC_DIR}/server/handlers/game_handler.cpp"
    "${SRC_DIR}/server/handlers/move_handler.cpp"
    "${SRC_DIR}/server/ecs/systems/map_grid.cpp"
    "${SRC_DIR}/server/ecs/systems/aoi_view_bridge.cpp"
    "${SRC_DIR}/server/ecs/systems/aoi_system.cpp"
    "${SRC_DIR}/server/ecs/systems/aoi_sector.cpp"
    "${SRC_DIR}/server/ecs/systems/world_system.cpp"
    "${SRC_DIR}/server/session/connection_system.cpp"
    "${SRC_DIR}/server/session/account_system.cpp"
    "${SRC_DIR}/server/session/role_system.cpp"
    "${SRC_DIR}/server/ecs/systems/map_system.cpp"
    "${SRC_DIR}/server/ecs/systems/map_config_system.cpp"
    "${SRC_DIR}/server/ecs/systems/player_entity_system.cpp"
    "${SRC_DIR}/server/ecs/systems/move_system.cpp"
    "${SRC_DIR}/server/ecs/systems/jolt_system.cpp"
    "${SRC_DIR}/server/ecs/components/move_component.cpp"
    "${SRC_DIR}/server/ecs/systems/player_persist_system.cpp"
    $<TARGET_OBJECTS:zrpc>
    $<TARGET_OBJECTS:game_proto>
)
target_include_directories(svc_game_3d_protocol_mass_test PRIVATE ${INC_DIRS})
target_link_directories(svc_game_3d_protocol_mass_test PRIVATE ${PROTOBUF_LINK_DIRS})
set_target_properties(svc_game_3d_protocol_mass_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${OUT_DIR}")
target_compile_options(svc_game_3d_protocol_mass_test PRIVATE -fpermissive -w -march=native)
target_link_libraries(svc_game_3d_protocol_mass_test PRIVATE jolt_physics ${PROTOBUF_LIB} pthread)
add_dependencies(svc_game_3d_protocol_mass_test gen_proto_lookup)
target_link_libraries(svc_game_3d_protocol_mass_test PRIVATE mongo)

# ============================================================================
# 5. svc_game_3d_client
# ============================================================================
add_executable(svc_game_3d_client
    "${SRC_DIR}/client/main.cc"
    $<TARGET_OBJECTS:zrpc>
    $<TARGET_OBJECTS:game_proto>
)
target_include_directories(svc_game_3d_client PRIVATE ${INC_DIRS})
target_link_directories(svc_game_3d_client PRIVATE ${PROTOBUF_LINK_DIRS})
target_link_libraries(svc_game_3d_client PRIVATE ${PROTOBUF_LIB} pthread)
set_target_properties(svc_game_3d_client PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${OUT_DIR}")
target_compile_options(svc_game_3d_client PRIVATE -fpermissive -w)

# ============================================================================
# 5a. aoi_enter_leave_test
# ============================================================================
add_executable(aoi_enter_leave_test
    "${SRC_DIR}/client/aoi_enter_leave_test.cc"
    $<TARGET_OBJECTS:zrpc>
    $<TARGET_OBJECTS:game_proto>
)
target_include_directories(aoi_enter_leave_test PRIVATE ${INC_DIRS})
target_link_directories(aoi_enter_leave_test PRIVATE ${PROTOBUF_LINK_DIRS})
target_link_libraries(aoi_enter_leave_test PRIVATE ${PROTOBUF_LIB} pthread)
set_target_properties(aoi_enter_leave_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${OUT_DIR}")
target_compile_options(aoi_enter_leave_test PRIVATE -fpermissive -w)

# ============================================================================
# 6. svc_game_3d_stress_test
# ============================================================================
add_executable(svc_game_3d_stress_test
    "${SRC_DIR}/client/stress_test.cc"
    $<TARGET_OBJECTS:zrpc>
    $<TARGET_OBJECTS:game_proto>
)
target_include_directories(svc_game_3d_stress_test PRIVATE ${INC_DIRS})
target_link_directories(svc_game_3d_stress_test PRIVATE ${PROTOBUF_LINK_DIRS})
target_link_libraries(svc_game_3d_stress_test PRIVATE ${PROTOBUF_LIB} pthread)
set_target_properties(svc_game_3d_stress_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${OUT_DIR}")
target_compile_options(svc_game_3d_stress_test PRIVATE -fpermissive -w)

# ============================================================================
# 7. pingpong 压测工具 (纯 TCP, 不依赖 protobuf)
# ============================================================================
add_executable(svc_game_3d_pingpong_server
    "${SRC_DIR}/client/server.cc"
    $<TARGET_OBJECTS:zrpc>
)
target_include_directories(svc_game_3d_pingpong_server PRIVATE ${INC_DIRS})
target_link_libraries(svc_game_3d_pingpong_server PRIVATE pthread)
set_target_properties(svc_game_3d_pingpong_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${OUT_DIR}")
target_compile_options(svc_game_3d_pingpong_server PRIVATE -fpermissive -w)

add_executable(svc_game_3d_pingpong_client
    "${SRC_DIR}/client/client.cc"
    $<TARGET_OBJECTS:zrpc>
)
target_include_directories(svc_game_3d_pingpong_client PRIVATE ${INC_DIRS})
target_link_libraries(svc_game_3d_pingpong_client PRIVATE pthread)
set_target_properties(svc_game_3d_pingpong_client PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${OUT_DIR}")
target_compile_options(svc_game_3d_pingpong_client PRIVATE -fpermissive -w)

# ============================================================================
# 8. EventLoop pipe 性能基准
# ============================================================================
add_executable(svc_game_3d_bench
    "${SRC_DIR}/client/bench.cc"
    $<TARGET_OBJECTS:zrpc>
)
target_include_directories(svc_game_3d_bench PRIVATE ${INC_DIRS})
target_link_libraries(svc_game_3d_bench PRIVATE pthread)
set_target_properties(svc_game_3d_bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${OUT_DIR}")
target_compile_options(svc_game_3d_bench PRIVATE -fpermissive -w)

# ============================================================================
# 汇总
# ============================================================================
message(STATUS ">>> ============================================")
message(STATUS ">>> 配置完成, 构建命令:")
message(STATUS ">>>   make -j\$(nproc)            # 全部")
message(STATUS ">>>   make svc_game_3d_server     # 单目标")
message(STATUS ">>>   make clean                  # 清理 .o")
message(STATUS ">>> 产物目录: ${OUT_DIR}")
message(STATUS ">>> ============================================")
