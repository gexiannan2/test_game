# gen_proto_lookup.cmake
#
# 从 Msg_Cli_protobuf.lua 解析 "name = id" 对，生成 proto_name_lookup.h。
# 生成头文件提供:
#     namespace proto_name { std::string lookup(uint32_t id); }
# 与 src/protocol/pack_flags.h 中 proto_name::lookup(id) 调用对应。
#
# 调用方式 (-P 模式):
#   cmake -DPROTO_LUA_FILE=<...> -DPROTO_LOOKUP_H=<...> -P gen_proto_lookup.cmake

if(NOT DEFINED PROTO_LUA_FILE OR NOT DEFINED PROTO_LOOKUP_H)
    message(FATAL_ERROR "PROTO_LUA_FILE 和 PROTO_LOOKUP_H 必须定义")
endif()

if(NOT EXISTS "${PROTO_LUA_FILE}")
    message(FATAL_ERROR "输入 lua 文件不存在: ${PROTO_LUA_FILE}")
endif()

file(READ "${PROTO_LUA_FILE}" _lua_content)

# 按行拆分
string(REPLACE "\n" ";" _lines "${_lua_content}")
string(REPLACE "\r" "" _lua_content "${_lua_content}")
string(REPLACE "\n" ";" _lines "${_lua_content}")

set(_entries "")
set(_count 0)
foreach(_line IN LISTS _lines)
    # 匹配 "    name = id," 形式（前导空白可选，尾随逗号可选）
    string(STRIP "${_line}" _stripped)
    if(_stripped MATCHES "^([A-Za-z_][A-Za-z0-9_]*)[ \t]*=[ \t]*([0-9]+),?$")
        set(_name "${CMAKE_MATCH_1}")
        set(_id "${CMAKE_MATCH_2}")
        string(APPEND _entries "    {${_id}, \"${_name}\"},\n")
        math(EXPR _count "${_count} + 1")
    endif()
endforeach()

# 生成头文件内容
set(_content
"// AUTO-GENERATED FILE — DO NOT EDIT.
// 由 cmake/gen_proto_lookup.cmake 从 Msg_Cli_protobuf.lua 生成。
// 重新生成: cmake --build <build_dir> --target gen_proto_lookup

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace proto_name {

inline const std::unordered_map<uint32_t, std::string>& table() {
    static const std::unordered_map<uint32_t, std::string> kTable = {
${_entries}    };
    return kTable;
}

// msg_id -> 可读名称; 未知 id 返回 \"<unknown:0x...>\"。
inline std::string lookup(uint32_t id) {
    const auto& t = table();
    auto it = t.find(id);
    if (it != t.end()) {
        return it->second;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), \"<unknown:%u>\", id);
    return buf;
}

}  // namespace proto_name
")

# 仅在内容变化时写盘，避免不必要的重新编译
if(EXISTS "${PROTO_LOOKUP_H}")
    file(READ "${PROTO_LOOKUP_H}" _old_content)
    if(_old_content STREQUAL _content)
        message(STATUS "proto_name_lookup.h 已是最新 (entries=${_count})")
        return()
    endif()
endif()

file(WRITE "${PROTO_LOOKUP_H}" "${_content}")
message(STATUS "已生成 ${PROTO_LOOKUP_H} (entries=${_count})")
