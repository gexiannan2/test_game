// aoi_enter_leave_test.cc — AOI 进出视野事件驱动测试

#include <google/protobuf/message.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "client_3d.pb.h"
#include "client_common.pb.h"
#include "client_login.pb.h"
#include "protocol/pack_codec.h"
#include "protocol/pack_flags.h"
#include "zrpc/base/buffer.h"
#include "zrpc/base/logger.h"
#include "zrpc/net/event_loop.h"
#include "zrpc/net/socket.h"
#include "zrpc/net/tcp_client.h"
#include "zrpc/net/tcp_connection.h"

#include "test_client.h"

#ifdef _WIN32
#include <winsock2.h>
#endif

using namespace zrpc;
using namespace aoitest;

#define LOG_TEST() (std::cout << "[TEST ] ")

namespace {

// 从 entity_3d 提取坐标：
//   - PLAYER_DATA(appears_ntf 进视野) → entity_player_data.base.pos
//   - MOVE_DATA(update_ntf 移动同步)  → entity_move_data.pos (顶层, 无 base 包装)
bool ExtractPlayerPos(const ::entity_3d& e3d, float* x, float* y, float* z)
{
    for (const auto& ed : e3d.entity_data_list())
    {
        if (ed.type() == ::ENTITY_DATA_TYPE_PLAYER_DATA)
        {
            ::entity_player_data pd;
            if (pd.ParseFromString(ed.data()) && pd.has_base() && pd.base().has_pos())
            {
                *x = pd.base().pos().x();
                *y = pd.base().pos().y();
                *z = pd.base().pos().z();
                return true;
            }
        }
        else if (ed.type() == ::ENTITY_DATA_TYPE_MOVE_DATA)
        {
            ::entity_move_data md;
            if (md.ParseFromString(ed.data()) && md.has_pos())
            {
                *x = md.pos().x();
                *y = md.pos().y();
                *z = md.pos().z();
                return true;
            }
        }
    }
    return false;
}

struct AppearEntry {
    uint64_t entity_id = 0;
    bool is_self = false;
    float x = 0, y = 0, z = 0;
};

// AOI 消息统计：记录客户端收到的 appears/disappears 明细，供校验和打印
struct AoiStats {
    std::string name;
    int appear_cnt = 0;           // 收到 cli_3d_aoi_appears_ntf 的条数
    int disappear_cnt = 0;        // 收到 cli_3d_aoi_disappears_ntf 的条数
    uint64_t first_appear_ins = 0;  // 第一条 appear 消息的首个实体 id
    uint64_t first_disappear_ins = 0; // 第一条 disappear 消息的首个实体 id
    float appear_x = 0, appear_y = 0, appear_z = 0; // 第一条 appear 首实体的坐标
    // 每条 appears_ntf 消息内的实体明细（用于逐条展示）
    std::vector<std::vector<AppearEntry>> appear_msgs;

    void Reset()
    {
        appear_cnt = 0;
        disappear_cnt = 0;
        first_appear_ins = 0;
        first_disappear_ins = 0;
        appear_x = appear_y = appear_z = 0;
        appear_msgs.clear();
    }

    // 统计某个实体在所有 appear 消息中出现的次数
    int CountAppearForEntity(uint64_t eid) const
    {
        int n = 0;
        for (const auto& msg : appear_msgs)
        {
            for (const auto& e : msg)
            {
                if (e.entity_id == eid) ++n;
            }
        }
        return n;
    }

    // 判断某实体是否以 is_self=true 出现过
    bool HasSelfAppear(uint64_t eid) const
    {
        for (const auto& msg : appear_msgs)
        {
            for (const auto& e : msg)
            {
                if (e.entity_id == eid && e.is_self) return true;
            }
        }
        return false;
    }

    void Install(TestClient& c)
    {
        auto queue = c.msg_queue();
        queue->SetOnPush([this](uint32_t msg_id, const std::string& body) {
            if (msg_id == proto_id("cli_3d_aoi_appears_ntf"))
            {
                ++appear_cnt;
                ::cli_3d_aoi_appears_ntf appear;
                if (!appear.ParseFromString(body)) return;
                std::vector<AppearEntry> msg;
                for (int i = 0; i < appear.list_size(); ++i)
                {
                    const auto& e3d = appear.list(i);
                    AppearEntry e;
                    e.entity_id = e3d.entity_id();
                    e.is_self = e3d.is_self();
                    ExtractPlayerPos(e3d, &e.x, &e.y, &e.z);
                    msg.push_back(e);
                }
                if (first_appear_ins == 0 && appear.list_size() > 0)
                {
                    first_appear_ins = appear.list(0).entity_id();
                    ExtractPlayerPos(appear.list(0), &appear_x, &appear_y, &appear_z);
                }
                appear_msgs.push_back(std::move(msg));
            }
            if (msg_id == proto_id("cli_3d_aoi_disappears_ntf"))
            {
                ++disappear_cnt;
                ::cli_3d_aoi_disappears_ntf dis;
                if (dis.ParseFromString(body) && first_disappear_ins == 0
                    && dis.entity_id_list_size() > 0)
                {
                    first_disappear_ins = dis.entity_id_list(0);
                }
            }
        });
    }

    void Print() const
    {
        std::cout << "  [" << name << "] 收到appear消息数=" << appear_cnt;
        if (appear_cnt > 0)
        {
            std::cout << " 首条实体id=" << first_appear_ins
                      << " 坐标=(" << appear_x << "," << appear_y
                      << "," << appear_z << ")";
        }
        std::cout << "  收到disappear消息数=" << disappear_cnt;
        if (disappear_cnt > 0)
        {
            std::cout << " 首条实体id=" << first_disappear_ins;
        }
        std::cout << std::endl;
        // 逐条打印每条 appears_ntf 消息内的实体明细
        for (size_t i = 0; i < appear_msgs.size(); ++i)
        {
            std::cout << "    [" << name << "] appear消息#" << (i + 1)
                      << " 包含实体数=" << appear_msgs[i].size() << ":";
            for (const auto& e : appear_msgs[i])
            {
                std::cout << " {实体id=" << e.entity_id
                          << " 是否自身=" << (e.is_self ? 1 : 0)
                          << " 坐标=(" << e.x << "," << e.y << "," << e.z << ")}";
            }
            std::cout << std::endl;
        }
    }
};

}  // namespace

int main(int argc, char** argv)
{
    std::string ip = "10.23.0.99";
    int port = 20002;
    if (argc >= 2) ip = argv[1];
    if (argc >= 3) port = std::atoi(argv[2]);

    std::cout << std::endl;
    LOG_TEST() << "===== AOI 进出视野测试（事件驱动）=====" << std::endl;
    LOG_TEST() << "服务器地址: " << ip << ":" << port << std::endl;
    LOG_TEST() << "AOI参数: 格子大小=10世界单位 视野半径=1格(即跨1格=10单位触发进出视野)" << std::endl;

    LOG_TEST() << "---- 阶段1: A先进图, 再让B进图 ----" << std::endl;

    TestClient a(ip, port, "user_aoi_A");
    TestClient b(ip, port, "user_aoi_B");

    AoiStats sa, sb;
    sa.name = "A"; sb.name = "B";
    sa.Install(a);
    sb.Install(b);

    // --- 步骤1: A 先进图 ---
    a.Start();
    if (!WaitForInGame(a)) { LOG_TEST() << "失败: A 进图超时" << std::endl; return 1; }
    LOG_TEST() << "A 进图成功 (role_id=" << a.RoleId() << ")" << std::endl;

    // A 应收到自身 appear（is_self=true），且仅 1 条
    a.msg_queue()->WaitFor(proto_id("cli_3d_aoi_appears_ntf"), 5000);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    sa.Print();
    {
        int self_cnt = sa.CountAppearForEntity(a.RoleId());
        bool self_flag = sa.HasSelfAppear(a.RoleId());
        LOG_TEST() << "  [校验1] A收到自身appear次数=" << self_cnt
                   << " 是否标记is_self=" << self_flag
                   << (self_cnt == 1 && self_flag ? " 通过" : " 失败") << std::endl;
        if (self_cnt != 1 || !self_flag)
        {
            LOG_TEST() << "失败: A 应恰好收到1次自身appear且is_self=true" << std::endl;
            a.Stop();
            return 1;
        }
    }

    // --- 步骤2: B 进图 ---
    b.Start();
    if (!WaitForInGame(b)) { LOG_TEST() << "失败: B 进图超时" << std::endl; return 1; }
    LOG_TEST() << "B 进图成功 (role_id=" << b.RoleId() << ")" << std::endl;

    // A 应收到 B 的 appear，但不再收到 A 的 appear
    a.msg_queue()->WaitFor(proto_id("cli_3d_aoi_appears_ntf"), 5000);
    b.msg_queue()->WaitFor(proto_id("cli_3d_aoi_appears_ntf"), 5000);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    sa.Print(); sb.Print();
    {
        int a_sees_b = sa.CountAppearForEntity(b.RoleId());
        int a_sees_a = sa.CountAppearForEntity(a.RoleId());
        LOG_TEST() << "  [校验2] B进图后: A看到B次数=" << a_sees_b
                   << " A看到A次数=" << a_sees_a
                   << " (期望 B>=1, A==1)"
                   << (a_sees_b >= 1 && a_sees_a == 1 ? " 通过" : " 失败") << std::endl;
        if (a_sees_b < 1 || a_sees_a != 1)
        {
            LOG_TEST() << "失败: A 应看到B, 但不应再次看到A" << std::endl;
            a.Stop(); b.Stop();
            return 1;
        }
    }

    float born_x = sb.appear_x;
    float born_y = sb.appear_y;
    float born_z = sb.appear_z;
    if (born_x == 0 && born_y == 0 && born_z == 0)
    {
        born_x = sa.appear_x; born_y = sa.appear_y; born_z = sa.appear_z;
    }

    // 验证 update_ntf：B 在同格内移动 2 次（不跨格），A 应收到 update_ntf，
    // 且每条 update 的 entity_data_list_size 必须为 1（不重复打包多条数据）
    LOG_TEST() << "  [阶段1-移动测试] B在原格子内移动2次, A应收到"
               << " cli_3d_aoi_update_ntf (每条data_list_size必须为1)" << std::endl;
    a.msg_queue()->Drain(); b.msg_queue()->Drain();
    int update_cnt = 0;
    int bad_update = 0;
    uint32_t upd_id = proto_id("cli_3d_aoi_update_ntf");
    // 打印某客户端收到的 update_ntf 明细：协议名 / entity_id / 坐标 / data_list_size
    auto dump_update = [](TestClient& c, const char* who, uint32_t uid,
                          int& cnt, int& bad) -> bool {
        ::cli_3d_aoi_update_ntf upd;
        if (!TakeFirst(*c.msg_queue(), uid, &upd)) return false;
        ++cnt;
        for (int j = 0; j < upd.list_size(); ++j)
        {
            const auto& e3d = upd.list(j);
            int ds = e3d.entity_data_list_size();
            float px = 0, py = 0, pz = 0;
            ExtractPlayerPos(e3d, &px, &py, &pz);
            LOG_TEST() << "    " << who << "收到 <- cli_3d_aoi_update_ntf:"
                       << " 实体id=" << e3d.entity_id()
                       << " 坐标=(" << px << "," << py << "," << pz << ")"
                       << " data_list_size=" << ds
                       << (ds == 1 ? " 正常" : " *** 异常>1 ***") << std::endl;
            if (ds != 1) ++bad;
        }
        return true;
    };

    for (int i = 0; i < 2; ++i)
    {
        float mx = born_x + static_cast<float>(i + 1) * 0.5f;
        LOG_TEST() << "  移动#" << (i + 1) << ": B(role_id=" << b.RoleId()
                   << ") 发送移动 -> 坐标(" << mx << "," << born_y
                   << "," << born_z << ")" << std::endl;
        b.SendMove(mx, born_y, born_z);
        // 收集 1s 内 A/B 收到的 update_ntf，不阻塞
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
        bool got = false;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (dump_update(a, "A", upd_id, update_cnt, bad_update)) got = true;
            if (dump_update(b, "B", upd_id, update_cnt, bad_update)) got = true;
            if (got) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    LOG_TEST() << "  [阶段1-移动测试] update_ntf汇总: A共收到 " << update_cnt
               << " 条update_ntf, 异常(data_list_size>1)=" << bad_update
               << (bad_update == 0 ? "  通过" : "  失败") << std::endl;

    a.msg_queue()->Drain(); b.msg_queue()->Drain();

    // 计算 AOI 跨格边界：born 是 B 出生坐标，cell 是所在格子索引(每格10单位)。
    // cross_out = 跨出 AOI 视野的 x 坐标(进入 cell+2 格，B 离开 A 视野 → disappear)
    // cross_in  = 跨回 AOI 视野的 x 坐标(回到 cell+1 格，B 重新进入 A 视野 → re-appear)
    int born_cell_x = static_cast<int>(std::floor(born_x / 10.0f));
    float cross_out = (born_cell_x + 2) * 10.0f;
    float cross_in  = cross_out - 10.0f;
    LOG_TEST() << "  [AOI边界计算] B出生坐标=(" << born_x << "," << born_y
               << "," << born_z << ") 所在格子#" << born_cell_x
               << " | 移动到x=" << cross_in << "(格子#" << (born_cell_x + 1)
               << ", 仍在视野内) | 移动到x=" << cross_out
               << "(格子#" << (born_cell_x + 2) << ", 跨出视野触发disappear)"
               << std::endl;

    bool ok = (sa.appear_cnt >= 1) && (sb.appear_cnt >= 1);
    LOG_TEST() << "  阶段1结果: " << (ok ? "通过" : "失败") << std::endl;

    a.msg_queue()->Drain(); b.msg_queue()->Drain();
    sa.Reset(); sb.Reset();
    b.SendMove(cross_in, born_y, born_z);
    b.WaitForMoveRes(5000);
    LOG_TEST() << "  [预备移动] B移动到x=" << cross_in
               << "(格子#" << (born_cell_x + 1) << ", 在A视野内, 预期不触发disappear)"
               << std::endl;

    LOG_TEST() << "---- 阶段2: B移出视野 -> 触发disappear ----" << std::endl;
    a.msg_queue()->Drain(); b.msg_queue()->Drain();
    sa.Reset(); sb.Reset();
    b.SendMove(cross_out, born_y, born_z);
    LOG_TEST() << "  B移动 (x=" << cross_in << "->" << cross_out
               << ") 跨出1格 -> 预期双方收到disappear" << std::endl;
    b.WaitForMoveRes(5000);

    bool a_dis = a.msg_queue()->WaitFor(proto_id("cli_3d_aoi_disappears_ntf"), 5000);
    bool b_dis = b.msg_queue()->WaitFor(proto_id("cli_3d_aoi_disappears_ntf"), 5000);
    sa.Print(); sb.Print();
    LOG_TEST() << "  A收到disappear=" << a_dis << "  B收到disappear=" << b_dis << std::endl;
    if (!a_dis || !b_dis) LOG_TEST() << "失败: 缺少disappear消息" << std::endl;

    LOG_TEST() << "---- 阶段3: B移回视野 -> 触发re-appear ----" << std::endl;
    a.msg_queue()->Drain(); b.msg_queue()->Drain();
    sa.Reset(); sb.Reset();
    b.SendMove(cross_in, born_y, born_z);
    LOG_TEST() << "  B移动 (x=" << cross_out << "->" << cross_in
               << ") 跨回1格 -> 预期双方收到re-appear" << std::endl;
    b.WaitForMoveRes(5000);

    bool a_reapp = a.msg_queue()->WaitFor(proto_id("cli_3d_aoi_appears_ntf"), 8000);
    bool b_reapp = b.msg_queue()->WaitFor(proto_id("cli_3d_aoi_appears_ntf"), 8000);
    sa.Print(); sb.Print();
    LOG_TEST() << "  A收到re-appear=" << a_reapp << "  B收到re-appear=" << b_reapp << std::endl;

    ok = ok && a_dis && b_dis && a_reapp && b_reapp;
    LOG_TEST() << "===== 最终结果: " << (ok ? "通过" : "失败") << " =====" << std::endl;

    a.Stop();
    b.Stop();
    return ok ? 0 : 1;
}
