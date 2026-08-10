#include "e996_msg.h"

#include "client_battle.pb.h"

namespace client_battle
{

    // 注册所有消息
    int reg_msg()
    {
        // 开始注册消息
        msg_reg_normal(cli_3d_skill_req);
        msg_reg_normal(cli_3d_hit_ntf);
        msg_reg_normal(cli_3d_skill_res);

        return 0;
    }

    auto _ = reg_msg();
}
