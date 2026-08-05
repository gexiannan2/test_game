include(CMakeFindDependencyMacro)
find_dependency(bson 2.3.3)
include("${CMAKE_CURRENT_LIST_DIR}/bsoncxx_targets.cmake")
