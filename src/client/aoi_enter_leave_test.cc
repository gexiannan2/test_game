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

// 从 entity_3d 提取 player_data 的 pos
bool ExtractPlayerPos(const ::entity_3d& e3d, float* x, float* y, float* z) {
    for (const auto& ed : e3d.entity_data_list()) {
        if (ed.type() == ::ENTITY_DATA_TYPE_PLAYER_DATA) {
            ::entity_player_data pd;
            if (pd.ParseFromString(ed.data()) && pd.has_base() && pd.base().has_pos()) {
                *x = pd.base().pos().x();
                *y = pd.base().pos().y();
                *z = pd.base().pos().z();
                return true;
            }
        }
    }
    return false;
}

struct AoiStats {
    std::string name;
    int appear_cnt = 0;
    int disappear_cnt = 0;
    uint64_t first_appear_ins = 0;
    uint64_t first_disappear_ins = 0;
    float appear_x = 0, appear_y = 0, appear_z = 0;

    void Install(TestClient& c) {
        auto queue = c.msg_queue();
        queue->SetOnPush([this](uint32_t msg_id, const std::string& body) {
            // 所有 ntf 统一打印 msg_id 名称和完整 DebugString
            std::string pname = msg_id_name(msg_id);
            bool is_ntf = (pname.find("_ntf") != std::string::npos);
            if (is_ntf) {
                std::cout << "\n  [" << name << "] ===== RECV " << pname
                          << "  body=" << body.size() << "B =====" << std::endl;
            }

            if (msg_id == proto_id("cli_3d_aoi_appears_ntf")) {
                ++appear_cnt;
                ::cli_3d_aoi_appears_ntf appear;
                if (!appear.ParseFromString(body)) {
                    std::cout << "  [" << name << "]   ParseFromString FAILED" << std::endl;
                    return;
                }
                // 打印完整协议内容
                std::cout << "  [" << name << "] DebugString:\n"
                          << appear.DebugString() << std::endl;
                std::cout << "  [" << name << "]   list_size=" << appear.list_size() << std::endl;
                for (int i = 0; i < appear.list_size(); ++i) {
                    const auto& e3d = appear.list(i);
                    std::cout << "  [" << name << "]   [" << i << "] entity_id=" << e3d.entity_id()
                              << " type=" << e3d.type()
                              << " is_self=" << e3d.is_self()
                              << " data_list_size=" << e3d.entity_data_list_size()
                              << std::endl;
                    for (const auto& ed : e3d.entity_data_list()) {
                        std::cout << "  [" << name << "]     data_type=" << ed.type()
                                  << " data_size=" << ed.data().size() << "B";
                        if (ed.type() == ::ENTITY_DATA_TYPE_PLAYER_DATA) {
                            ::entity_player_data pd;
                            if (pd.ParseFromString(ed.data())) {
                                std::cout << " parse=OK";
                                if (pd.has_base() && pd.base().has_pos()) {
                                    std::cout << " pos=(" << pd.base().pos().x()
                                              << "," << pd.base().pos().y()
                                              << "," << pd.base().pos().z() << ")";
                                } else {
                                    std::cout << " (no base/pos)";
                                }
                                std::cout << "\n  [" << name << "]     player_data DebugString:\n"
                                          << pd.DebugString();
                            } else {
                                std::cout << " parse=FAILED";
                            }
                        }
                        std::cout << std::endl;
                    }
                }
                if (first_appear_ins == 0 && appear.list_size() > 0) {
                    first_appear_ins = appear.list(0).entity_id();
                    ExtractPlayerPos(appear.list(0), &appear_x, &appear_y, &appear_z);
                    std::cout << "  [" << name << "]   first_appear=" << first_appear_ins
                              << " pos=(" << appear_x << "," << appear_y << "," << appear_z << ")"
                              << std::endl;
                }
            }
            if (msg_id == proto_id("cli_3d_aoi_disappears_ntf")) {
                ++disappear_cnt;
                ::cli_3d_aoi_disappears_ntf dis;
                if (!dis.ParseFromString(body)) {
                    std::cout << "  [" << name << "]   disappears ParseFromString FAILED" << std::endl;
                } else {
                    std::cout << "  [" << name << "] DebugString:\n"
                              << dis.DebugString() << std::endl;
                    if (first_disappear_ins == 0 && dis.entity_id_list_size() > 0) {
                        first_disappear_ins = dis.entity_id_list(0);
                    }
                }
                std::cout << "  [" << name << "] RECV disappears_ntf body=" << body.size() << "B" << std::endl;
            }
            if (msg_id == proto_id("cli_3d_aoi_update_ntf")) {
                ::cli_3d_aoi_update_ntf upd;
                if (!upd.ParseFromString(body)) {
                    std::cout << "  [" << name << "]   update ParseFromString FAILED" << std::endl;
                } else {
                    std::cout << "  [" << name << "] ===== RECV cli_3d_aoi_update_ntf"
                              << "  list_size=" << upd.list_size()
                              << "  body=" << body.size() << "B =====" << std::endl;
                    for (int i = 0; i < upd.list_size(); ++i) {
                        const auto& e3d = upd.list(i);
                        int dl_size = e3d.entity_data_list_size();
                        std::cout << "  [" << name << "]   [" << i << "] entity_id=" << e3d.entity_id()
                                  << " type=" << e3d.type()
                                  << " is_self=" << e3d.is_self()
                                  << " data_list_size=" << dl_size
                                  << (dl_size == 1 ? " (OK)" : " *** EXPECTED 1 ***")
                                  << std::endl;
                        for (const auto& ed : e3d.entity_data_list()) {
                            std::cout << "  [" << name << "]     data_type=" << ed.type()
                                      << " data_size=" << ed.data().size() << "B";
                            if (ed.type() == ::ENTITY_DATA_TYPE_MOVE_DATA) {
                                ::entity_move_data md;
                                if (md.ParseFromString(ed.data())) {
                                    std::cout << " pos=(" << md.pos().x() << ","
                                              << md.pos().y() << "," << md.pos().z() << ")"
                                              << " rot=(" << md.rot().x() << ","
                                              << md.rot().y() << "," << md.rot().z()
                                              << "," << md.rot().w() << ")"
                                              << " vel=(" << (md.has_velocity() ? md.velocity().x() : 0)
                                              << "," << (md.has_velocity() ? md.velocity().y() : 0)
                                              << "," << (md.has_velocity() ? md.velocity().z() : 0)
                                              << ")";
                                } else {
                                    std::cout << " move_data parse=FAILED";
                                }
                            }
                            std::cout << std::endl;
                        }
                    }
                }
            }
            // 其他 ntf（enter_map 等）也打印 DebugString
            if (is_ntf && msg_id != proto_id("cli_3d_aoi_appears_ntf")
                        && msg_id != proto_id("cli_3d_aoi_disappears_ntf")) {
                // 通用 Message 反射打印
                std::cout << "  [" << name << "] (raw body hex, first 64 bytes): ";
                for (size_t i = 0; i < body.size() && i < 64; ++i) {
                    printf("%02X ", (unsigned char)body[i]);
                }
                std::cout << std::endl;
            }
        });
    }

    void Print() const {
        std::cout << "  [" << name << "] appear=" << appear_cnt;
        if (appear_cnt > 0) {
            std::cout << " entity_id=" << first_appear_ins
                      << " pos=(" << appear_x << "," << appear_y
                      << "," << appear_z << ")";
        }
        std::cout << "  disappear=" << disappear_cnt;
        if (disappear_cnt > 0) {
            std::cout << " entity_id=" << first_disappear_ins;
        }
        std::cout << std::endl;
    }
};

}  // namespace

int main(int argc, char** argv) {
    std::string ip = "10.23.0.99";
    int port = 20002;
    if (argc >= 2) ip = argv[1];
    if (argc >= 3) port = std::atoi(argv[2]);

    std::cout << std::endl;
    LOG_TEST() << "AOI Enter/Leave/Re-Enter Test (event-driven)" << std::endl;
    LOG_TEST() << "server: " << ip << ":" << port << std::endl;
    LOG_TEST() << "kAoiCellWorldSize=10  kAoiRadius=1" << std::endl;

    LOG_TEST() << "---- Phase 1: A & B connect & enter game ----" << std::endl;

    TestClient a(ip, port, "user_aoi_A");
    TestClient b(ip, port, "user_aoi_B");

    AoiStats sa, sb;
    sa.name = "A"; sb.name = "B";
    sa.Install(a);
    sb.Install(b);

    a.Start();
    b.Start();

    if (!WaitForInGame(a)) { LOG_TEST() << "FAIL: A timeout" << std::endl; return 1; }
    if (!WaitForInGame(b)) { LOG_TEST() << "FAIL: B timeout" << std::endl; return 1; }
    LOG_TEST() << "A in-game (role_id=" << a.RoleId()
               << ")  B in-game (role_id=" << b.RoleId() << ")" << std::endl;

    a.msg_queue()->WaitFor(proto_id("cli_3d_aoi_appears_ntf"), 5000);
    b.msg_queue()->WaitFor(proto_id("cli_3d_aoi_appears_ntf"), 5000);
    sa.Print(); sb.Print();

    float born_x = sb.appear_x;
    float born_y = sb.appear_y;
    float born_z = sb.appear_z;
    if (born_x == 0 && born_y == 0 && born_z == 0) {
        born_x = sa.appear_x; born_y = sa.appear_y; born_z = sa.appear_z;
    }
    int born_cell_x = static_cast<int>(std::floor(born_x / 10.0f));
    float cross_out = (born_cell_x + 2) * 10.0f;
    float cross_in  = cross_out - 10.0f;
    LOG_TEST() << "  born=(" << born_x << "," << born_y << "," << born_z
               << ") cell=" << born_cell_x
               << " cross_out=" << cross_out << " cross_in=" << cross_in << std::endl;

    bool ok = (sa.appear_cnt >= 1) && (sb.appear_cnt >= 1);
    LOG_TEST() << "  result: " << (ok ? "PASS" : "FAIL") << std::endl;

    a.msg_queue()->Drain(); b.msg_queue()->Drain();
    sa.appear_cnt = 0; sa.disappear_cnt = 0;
    sb.appear_cnt = 0; sb.disappear_cnt = 0;
    b.SendMove(cross_in, born_y, born_z);
    b.WaitForMoveRes(5000);
    LOG_TEST() << "  pre-move: B to (" << cross_in << "," << born_y
               << "," << born_z << ") - still visible" << std::endl;

    LOG_TEST() << "---- Phase 2: B moves away -> disappear ----" << std::endl;
    a.msg_queue()->Drain(); b.msg_queue()->Drain();
    sa.appear_cnt = 0; sa.disappear_cnt = 0;
    sb.appear_cnt = 0; sb.disappear_cnt = 0;
    b.SendMove(cross_out, born_y, born_z);
    LOG_TEST() << "  B (" << cross_in << "->" << cross_out
               << ") cross +1 cell -> expect disappear" << std::endl;
    b.WaitForMoveRes(5000);

    bool a_dis = a.msg_queue()->WaitFor(proto_id("cli_3d_aoi_disappears_ntf"), 5000);
    bool b_dis = b.msg_queue()->WaitFor(proto_id("cli_3d_aoi_disappears_ntf"), 5000);
    sa.Print(); sb.Print();
    LOG_TEST() << "  A disappear=" << a_dis << "  B disappear=" << b_dis << std::endl;
    if (!a_dis || !b_dis) LOG_TEST() << "FAIL: missing disappear" << std::endl;

    LOG_TEST() << "---- Phase 3: B moves back -> re-appear ----" << std::endl;
    a.msg_queue()->Drain(); b.msg_queue()->Drain();
    sa.appear_cnt = 0; sa.disappear_cnt = 0;
    sb.appear_cnt = 0; sb.disappear_cnt = 0;
    b.SendMove(cross_in, born_y, born_z);
    LOG_TEST() << "  B (" << cross_out << "->" << cross_in
               << ") cross -1 cell -> expect re-appear" << std::endl;
    b.WaitForMoveRes(5000);

    bool a_reapp = a.msg_queue()->WaitFor(proto_id("cli_3d_aoi_appears_ntf"), 8000);
    bool b_reapp = b.msg_queue()->WaitFor(proto_id("cli_3d_aoi_appears_ntf"), 8000);
    sa.Print(); sb.Print();
    LOG_TEST() << "  A re-appear=" << a_reapp << "  B re-appear=" << b_reapp << std::endl;

    ok = ok && a_dis && b_dis && a_reapp && b_reapp;
    LOG_TEST() << "==== RESULT: " << (ok ? "PASS" : "FAIL") << " ====" << std::endl;

    a.Stop();
    b.Stop();
    return ok ? 0 : 1;
}
