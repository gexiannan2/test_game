#include "e996_msg.h"

#include "client_login.pb.h"

namespace client_login
{

    // 注册所有消息
    int reg_msg()
    {
        // 开始注册消息
        msg_reg_normal(cli_handshake_req);
        msg_reg_normal(cli_user_login_req);
        msg_reg_normal(cli_role_info);
        msg_reg_normal(cli_role_list_req);
        msg_reg_normal(cli_role_create_req);
        msg_reg_normal(cli_role_delete_req);
        msg_reg_normal(cli_role_login_req);
        msg_reg_normal(cli_enter_game_req);
        msg_reg_normal(cli_global_config_ntf);
        msg_reg_normal(cli_heart_beat_req);
        msg_reg_normal(cli_reconnect_req);
        msg_reg_normal(cli_random_name_req);
        msg_reg_normal(cli_kickoff_player_ntf);
        msg_reg_normal(cli_role_list_res);
        msg_reg_normal(cli_reconnect_res);
        msg_reg_normal(cli_role_login_res);
        msg_reg_normal(cli_heart_beat_res);
        msg_reg_normal(cli_random_name_res);
        msg_reg_normal(cli_handshake_res);
        msg_reg_normal(cli_enter_game_res);
        msg_reg_normal(cli_user_login_res);

        return 0;
    }

    auto _ = reg_msg();
}
