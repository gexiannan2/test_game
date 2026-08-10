#include "e996_msg.h"

#include "client_3d.pb.h"

namespace client_3d
{

    // 注册所有消息
    int reg_msg()
    {
        // 开始注册消息
        msg_reg_normal(cli_3d_move_req);
        msg_reg_normal(cli_3d_jump_req);
        msg_reg_normal(cli_3d_dodge_req);
        msg_reg_normal(cli_3d_aoi_appears_ntf);
        msg_reg_normal(cli_3d_aoi_update_ntf);
        msg_reg_normal(cli_3d_aoi_disappears_ntf);
        msg_reg_normal(cli_3d_aoi_attr_update_ntf);
        msg_reg_normal(cli_3d_aoi_animation_ntf);
        msg_reg_normal(cli_3d_enter_map_ntf);
        msg_reg_normal(cli_3d_leave_map_req);
        msg_reg_normal(cli_3d_leave_map_ntf);
        msg_reg_normal(cli_3d_move_res);
        msg_reg_normal(cli_3d_dodge_res);
        msg_reg_normal(cli_3d_leave_map_res);
        msg_reg_normal(cli_3d_jump_res);

        return 0;
    }

    auto _ = reg_msg();
}
