// AUTO-GENERATED FILE — DO NOT EDIT.
// 由 cmake/gen_proto_lookup.cmake 从 Msg_Cli_protobuf.lua 生成。
// 重新生成: cmake --build <build_dir> --target gen_proto_lookup

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace proto_name {

inline const std::unordered_map<uint32_t, std::string>& table() {
    static const std::unordered_map<uint32_t, std::string> kTable = {
    {1618264583, "cli_3d_move_req"},
    {2390401835, "cli_3d_move_res"},
    {1745805876, "cli_3d_jump_req"},
    {2248180504, "cli_3d_jump_res"},
    {4095762581, "cli_3d_dodge_req"},
    {439222713, "cli_3d_dodge_res"},
    {2427491104, "cli_3d_aoi_appears_ntf"},
    {790443799, "cli_3d_aoi_update_ntf"},
    {1689879943, "cli_3d_aoi_disappears_ntf"},
    {1037773180, "cli_3d_aoi_attr_update_ntf"},
    {151465209, "cli_3d_aoi_animation_ntf"},
    {3775577961, "cli_3d_enter_map_ntf"},
    {161648790, "cli_3d_leave_map_req"},
    {3886870970, "cli_3d_leave_map_res"},
    {3432279125, "cli_3d_leave_map_ntf"},
    {3201020581, "cli_3d_skill_req"},
    {1355142025, "cli_3d_skill_res"},
    {1089166412, "cli_3d_hit_ntf"},
    {2379479427, "entity_player_data"},
    {3588586235, "entity_move_data"},
    {1782407629, "entity_jump_data"},
    {4270827257, "entity_dodge_data"},
    {1734336967, "weapon_capsule"},
    {3625745799, "entity_skill_data"},
    {3846130248, "entity_data"},
    {237519976, "entity"},
    {3612649913, "entity_battle"},
    {154207190, "entity_appear_item"},
    {1307478207, "entity_base_data"},
    {3490476261, "vec3"},
    {2339593629, "quat"},
    {3245459587, "entity_3d"},
    {538625942, "cli_handshake_req"},
    {3457459898, "cli_handshake_res"},
    {2318793131, "cli_user_login_req"},
    {1681627271, "cli_user_login_res"},
    {1638724494, "cli_role_info"},
    {2444757349, "cli_role_list_req"},
    {1842376278, "cli_role_create_req"},
    {2897044911, "cli_role_delete_req"},
    {2142660681, "cli_role_list_res"},
    {3676415891, "cli_role_login_req"},
    {892323519, "cli_role_login_res"},
    {4273429063, "cli_enter_game_req"},
    {280574827, "cli_enter_game_res"},
    {2142539992, "cli_global_config_ntf"},
    {637663842, "cli_heart_beat_req"},
    {3356465998, "cli_heart_beat_res"},
    {1078738440, "cli_reconnect_req"},
    {2923585316, "cli_reconnect_res"},
    {1629262764, "cli_random_name_req"},
    {2400383616, "cli_random_name_res"},
    {1570018916, "cli_kickoff_player_ntf"},
    };
    return kTable;
}

// msg_id -> 可读名称; 未知 id 返回 "<unknown:0x...>"。
inline std::string lookup(uint32_t id) {
    const auto& t = table();
    auto it = t.find(id);
    if (it != t.end()) {
        return it->second;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "<unknown:%u>", id);
    return buf;
}

}  // namespace proto_name
