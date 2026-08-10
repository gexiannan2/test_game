#include "e996_msg.h"

#include "game.pb.h"

namespace game
{

    // 注册所有消息
    int reg_msg()
    {
        // 开始注册消息
        msg_reg_normal(cli_handshake_test_req);
        msg_reg_normal(cli_handshake_test_res);

        return 0;
    }

    auto _ = reg_msg();
}
