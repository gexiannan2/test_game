include_guard(GLOBAL)

include(FetchContent)

# 游戏服只使用 BehaviorTree.CPP 核心运行时。关闭上游工具、示例、测试与
# 可选日志后端，避免引入 ZeroMQ、SQLite 等服务端不需要的依赖。
set(BTCPP_SHARED_LIBS OFF CACHE BOOL "Build BehaviorTree.CPP as a static library" FORCE)
set(BTCPP_BUILD_TOOLS OFF CACHE BOOL "Build BehaviorTree.CPP tools" FORCE)
set(BTCPP_EXAMPLES OFF CACHE BOOL "Build BehaviorTree.CPP examples" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "Build dependency tests" FORCE)
set(BTCPP_GROOT_INTERFACE OFF CACHE BOOL "Enable Groot2 support" FORCE)
set(BTCPP_SQLITE_LOGGING OFF CACHE BOOL "Enable SQLite logging" FORCE)

# 完整源码随项目保存在 third_party，FetchContent 只负责按上游方式挂载目标。
# 保留仓库与标签声明，便于明确来源和后续审计升级。
set(FETCHCONTENT_SOURCE_DIR_BEHAVIORTREE_CPP
    "${CMAKE_SOURCE_DIR}/third_party/behaviortree_cpp"
    CACHE PATH "Vendored BehaviorTree.CPP source directory" FORCE
)

if(NOT EXISTS "${FETCHCONTENT_SOURCE_DIR_BEHAVIORTREE_CPP}/CMakeLists.txt")
    message(FATAL_ERROR
        "BehaviorTree.CPP source is missing: "
        "${FETCHCONTENT_SOURCE_DIR_BEHAVIORTREE_CPP}")
endif()

FetchContent_Declare(
    behaviortree_cpp
    GIT_REPOSITORY https://github.com/BehaviorTree/BehaviorTree.CPP.git
    GIT_TAG        4.10.0
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(behaviortree_cpp)
