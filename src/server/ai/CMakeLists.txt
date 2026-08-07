set(MONSTER_TREE_XML
    "${CMAKE_CURRENT_SOURCE_DIR}/behavior_tree/trees/basic_monster.xml")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${MONSTER_TREE_XML}")
file(READ "${MONSTER_TREE_XML}" MONSTER_TREE_XML_CONTENT)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/behavior_tree/basic_monster_tree_xml.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/basic_monster_tree_xml.h"
    @ONLY
)

add_library(game_monster_ai STATIC
    "behavior_tree/monster_ai_nodes.cpp"
    "behavior_tree/monster_ai_system.cpp"
    "${MONSTER_TREE_XML}"
)

target_include_directories(game_monster_ai
    PUBLIC
        "${SRC_DIR}"
        "${SRC_DIR}/server"
    PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
        "${JOLT_DIR}/include"
        "${PROTO_GEN_DIR}"
        "${PROTOBUF_ROOT}/include"
)

target_link_libraries(game_monster_ai
    PUBLIC
        BT::behaviortree_cpp
)

target_compile_features(game_monster_ai PUBLIC cxx_std_20)
