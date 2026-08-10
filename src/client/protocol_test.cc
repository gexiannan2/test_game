// protocol_test.cc — 端到端协议测试（loopback GameServer + TcpClient）
// 覆盖：进图 appear、离图 disappear、跨格移动 AOI、enter_map、appear 字段完整性

#include <google/protobuf/message.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "client_3d.pb.h"
#include "client_common.pb.h"
#include "client_login.pb.h"
#include "protocol/pack_codec.h"
#include "protocol/pack_flags.h"
#include "common/init.h"
#include "zrpc/base/buffer.h"
#include "zrpc/base/logger.h"
#include "zrpc/net/event_loop.h"
#include "zrpc/net/socket.h"
#include "zrpc/net/tcp_client.h"
#include "zrpc/net/tcp_connection.h"

// 服务端（链接 server 源码）
#include "game_server.h"

#ifdef _WIN32
#include <winsock2.h>
#endif

using namespace zrpc;

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

void SendFrame(const std::shared_ptr<TcpConnection>& conn, uint32_t msg_id,
               const google::protobuf::Message& msg, uint8_t* seq) {
    PackFrame frame;
    frame.flags = kPackFlagEncrypt;
    frame.msg_id = msg_id;
    frame.recv_index = (*seq)++;
    msg.SerializeToString(&frame.body);
    Buffer out;
    EncodeFrame(frame, &out);
    conn->Send(&out);
}

struct ReceivedMsg {
    uint32_t msg_id = 0;
    std::string body;
};

class MsgQueue {
 public:
    void Push(uint32_t msg_id, const std::string& body) {
        std::lock_guard<std::mutex> lk(mu_);
        msgs_.push_back({msg_id, body});
        cv_.notify_all();
    }

    std::vector<ReceivedMsg> Drain() {
        std::lock_guard<std::mutex> lk(mu_);
        return std::move(msgs_);
    }

    size_t Count(uint32_t msg_id) {
        std::lock_guard<std::mutex> lk(mu_);
        size_t n = 0;
        for (auto& m : msgs_) {
            if (m.msg_id == msg_id) ++n;
        }
        return n;
    }

    bool WaitFor(uint32_t msg_id, int timeout_ms) {
        std::unique_lock<std::mutex> lk(mu_);
        return cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] {
            for (auto& m : msgs_) {
                if (m.msg_id == msg_id) return true;
            }
            return false;
        });
    }

    bool WaitForCount(uint32_t msg_id, size_t want, int timeout_ms) {
        std::unique_lock<std::mutex> lk(mu_);
        return cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] {
            size_t n = 0;
            for (auto& m : msgs_) {
                if (m.msg_id == msg_id) ++n;
            }
            return n >= want;
        });
    }

 private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<ReceivedMsg> msgs_;
};

class TestServer {
 public:
    bool Start(int port) {
        port_ = port;
        thread_ = std::thread([this] {
            server_ = std::make_unique<GameServer>("127.0.0.1", port_);
            server_->Start();
            server_->Loop();
            server_.reset();
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        return true;
    }

    void Stop() {
        GameServer* s = server_.get();
        if (s) s->Stop();
        if (thread_.joinable()) thread_.join();
    }

    GameServer* Get() { return server_.get(); }

 private:
    int port_ = 0;
    std::unique_ptr<GameServer> server_;
    std::thread thread_;
};

class TestClient {
 public:
    TestClient(const std::string& ip, int port, const std::string& uid)
        : ip_(ip), port_(port), uid_(uid), msg_queue_(std::make_shared<MsgQueue>()) {}

    ~TestClient() { Stop(); }

    void Start() {
        thread_ = std::thread([this] {
            EventLoop loop;
            TcpClient client(&loop, ip_, port_, nullptr);
            loop_ = &loop;
            client_ = &client;
            client.SetConnectionCallback(
                std::bind(&TestClient::OnConnection, this, std::placeholders::_1));
            client.SetMessageCallback(
                std::bind(&TestClient::OnMessage, this, std::placeholders::_1,
                          std::placeholders::_2));
            client.Connect();
            {
                std::lock_guard<std::mutex> lk(start_mu_);
                ready_ = true;
                start_cv_.notify_one();
            }
            loop.Run();
        });
        std::unique_lock<std::mutex> lk(start_mu_);
        start_cv_.wait(lk, [this] { return ready_; });
    }

    void Stop() {
        if (stopped_.exchange(true)) return;
        if (loop_) {
            loop_->RunInLoop([this] {
                if (conn_ && conn_->Connected()) {
                    conn_->Shutdown();
                }
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            loop_->Quit();
        }
        if (thread_.joinable()) thread_.join();
        loop_ = nullptr;
        client_ = nullptr;
    }

    std::shared_ptr<MsgQueue> msg_queue() { return msg_queue_; }
    bool IsConnected() const { return connected_.load(); }
    bool IsInGame() const { return in_game_.load(); }
    uint64_t RoleId() const { return role_id_.load(); }

    bool WaitForMoveRes(int timeout_ms) {
        return msg_queue_->WaitFor(proto_id("cli_3d_move_res"), timeout_ms);
    }

    void SendMove(float x, float y, float z) {
        if (!loop_) return;
        loop_->RunInLoop([this, x, y, z] {
            if (!conn_ || !conn_->Connected()) return;
            ::cli_3d_move_req req;
            auto* m = req.mutable_move();
            auto* pos = m->mutable_pos();
            pos->set_x(x);
            pos->set_y(y);
            pos->set_z(z);
            auto* rot = m->mutable_rot();
            rot->set_x(0); rot->set_y(0); rot->set_z(0); rot->set_w(1);
            auto* vel = m->mutable_velocity();
            vel->set_x(1); vel->set_y(0); vel->set_z(0);
            auto* mrot = m->mutable_move_rot();
            mrot->set_x(0); mrot->set_y(0); mrot->set_z(0); mrot->set_w(1);
            m->set_status(0);
            SendFrame(conn_, proto_id("cli_3d_move_req"), req, &seq_);
        });
    }

 private:
    void OnConnection(const std::shared_ptr<TcpConnection>& conn) {
        if (conn->Connected()) {
            connected_ = true;
            conn_ = conn;
            socket::SetKeepAlive(conn->GetSockfd(), 1);
            ::cli_handshake_req req;
            req.set_version("protocol_test/1.0");
            SendFrame(conn_, proto_id("cli_handshake_req"), req, &seq_);
        } else {
            connected_ = false;
        }
    }

    void OnMessage(const std::shared_ptr<TcpConnection>& conn, Buffer* buf) {
        std::vector<PackFrame> frames;
        if (!TryDecodeFrames(buf, frames)) {
            conn->Shutdown();
            return;
        }
        for (const auto& f : frames) {
            HandleFrame(f);
        }
    }

    void HandleFrame(const PackFrame& f) {
        msg_queue_->Push(f.msg_id, f.body);
        if (f.msg_id == proto_id("cli_handshake_res")) {
            ::cli_handshake_res rsp;
            if (rsp.ParseFromString(f.body) && rsp.err_code() == 0) {
                ::cli_user_login_req req;
                req.set_uid(uid_);
                req.set_token("token");
                req.set_channel_id(1);
                SendFrame(conn_, proto_id("cli_user_login_req"), req, &seq_);
            }
        } else if (f.msg_id == proto_id("cli_user_login_res")) {
            ::cli_user_login_res rsp;
            if (rsp.ParseFromString(f.body) && rsp.err_code() == 0) {
                session_id_ = rsp.session_id();
                ::cli_role_list_req req;
                SendFrame(conn_, proto_id("cli_role_list_req"), req, &seq_);
            }
        } else if (f.msg_id == proto_id("cli_role_list_res")) {
            ::cli_role_list_res rsp;
            if (rsp.ParseFromString(f.body)) {
                if (rsp.role_list_size() > 0) {
                    role_id_ = rsp.role_list(0).role_id();
                    ::cli_role_login_req req;
                    req.set_role_id(role_id_);
                    req.set_op_code(1);
                    SendFrame(conn_, proto_id("cli_role_login_req"), req, &seq_);
                } else {
                    ::cli_role_create_req req;
                    auto* info = req.mutable_role_info();
                    info->set_name("TestPlayer_" + uid_);
                    info->set_sex(1);
                    info->set_job(1);
                    SendFrame(conn_, proto_id("cli_role_create_req"), req, &seq_);
                }
            }
        } else if (f.msg_id == proto_id("cli_role_login_res")) {
            ::cli_role_login_res rsp;
            if (rsp.ParseFromString(f.body) && rsp.err_code() == 0) {
                ::cli_enter_game_req req;
                SendFrame(conn_, proto_id("cli_enter_game_req"), req, &seq_);
            }
        } else if (f.msg_id == proto_id("cli_3d_enter_map_ntf")) {
            ::cli_3d_enter_map_ntf rsp;
            if (rsp.ParseFromString(f.body) && rsp.err_code() == 0) {
                in_game_ = true;
            }
        }
    }

    std::string ip_;
    int port_;
    std::string uid_;
    EventLoop* loop_ = nullptr;
    TcpClient* client_ = nullptr;
    std::mutex start_mu_;
    std::condition_variable start_cv_;
    bool ready_ = false;
    std::thread thread_;
    std::shared_ptr<TcpConnection> conn_;
    uint8_t seq_ = 0;
    uint64_t session_id_ = 0;
    std::atomic<uint64_t> role_id_{0};
    std::atomic<bool> connected_{false};
    std::atomic<bool> in_game_{false};
    std::atomic<bool> stopped_{false};
    std::shared_ptr<MsgQueue> msg_queue_;
};

int g_test_failures = 0;

#define PROTO_CHECK(cond, msg)                                             \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::cerr << "[FAIL] " << (msg) << " @ " << __FILE__ << ":"     \
                      << __LINE__ << std::endl;                             \
            ++g_test_failures;                                              \
        }                                                                   \
    } while (0)

bool WaitForInGame(TestClient& c, int timeout_ms = 8000) {
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start)
               .count() < timeout_ms) {
        if (c.IsInGame()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return c.IsInGame();
}

template <typename MsgT>
bool TakeFirst(MsgQueue& mb, uint32_t msg_id, MsgT* out) {
    auto msgs = mb.Drain();
    for (auto& m : msgs) {
        if (m.msg_id == msg_id) {
            return out->ParseFromString(m.body);
        }
    }
    return false;
}

// 测试 1：双玩家进图互相收到 appear + 自身收到 enter_map
bool Test_EnterMapAppearBroadcast() {
    g_test_failures = 0;
    std::cout << "\n[Test 1] EnterMapAppearBroadcast" << std::endl;
    const int port = 21001;

    TestServer server;
    server.Start(port);

    TestClient a("127.0.0.1", port, "user_A");
    TestClient b("127.0.0.1", port, "user_B");
    a.Start();
    b.Start();

    bool in_game = WaitForInGame(a) && WaitForInGame(b);
    PROTO_CHECK(in_game, "clients failed to enter game");

    if (in_game) {
        bool a_got = a.msg_queue()->WaitFor(proto_id("cli_3d_aoi_appears_ntf"), 5000);
        bool b_got = b.msg_queue()->WaitFor(proto_id("cli_3d_aoi_appears_ntf"), 5000);
        std::cout << "  A got appear=" << a_got
                  << "  B got appear=" << b_got << std::endl;
        PROTO_CHECK(a_got, "A did not receive appear from B");
        PROTO_CHECK(b_got, "B did not receive appear from A");

        size_t a_appear_cnt = a.msg_queue()->Count(proto_id("cli_3d_aoi_appears_ntf"));
        size_t b_appear_cnt = b.msg_queue()->Count(proto_id("cli_3d_aoi_appears_ntf"));
        std::cout << "  appear_count: A=" << a_appear_cnt
                  << "  B=" << b_appear_cnt << std::endl;

        bool a_enter = a.msg_queue()->WaitFor(proto_id("cli_3d_enter_map_ntf"), 5000);
        std::cout << "  A enter_map=" << a_enter << std::endl;
        PROTO_CHECK(a_enter, "A did not receive cli_3d_enter_map_ntf");
    }

    a.Stop();
    b.Stop();
    server.Stop();
    return g_test_failures == 0;
}

// 测试 2：B 断开后 A 收到 disappear
bool Test_LeaveMapDisappearBroadcast() {
    g_test_failures = 0;
    std::cout << "\n[Test 2] LeaveMapDisappearBroadcast" << std::endl;
    const int port = 21002;

    TestServer server;
    server.Start(port);

    TestClient a("127.0.0.1", port, "user_A2");
    TestClient b("127.0.0.1", port, "user_B2");
    a.Start();
    b.Start();

    bool in_game = WaitForInGame(a) && WaitForInGame(b);
    PROTO_CHECK(in_game, "clients failed to enter game");

    if (in_game) {
        a.msg_queue()->WaitFor(proto_id("cli_3d_aoi_appears_ntf"), 5000);
        b.msg_queue()->WaitFor(proto_id("cli_3d_aoi_appears_ntf"), 5000);
        std::cout << "  both in game, A appear_cnt=" << a.msg_queue()->Count(proto_id("cli_3d_aoi_appears_ntf"))
                  << " B appear_cnt=" << b.msg_queue()->Count(proto_id("cli_3d_aoi_appears_ntf")) << std::endl;
        a.msg_queue()->Drain();

        b.Stop();

        bool got = a.msg_queue()->WaitFor(proto_id("cli_3d_aoi_disappears_ntf"), 5000);
        std::cout << "  A got disappear=" << got << std::endl;
        PROTO_CHECK(got, "A did not receive disappear after B disconnected");
    }

    a.Stop();
    server.Stop();
    return g_test_failures == 0;
}

// 测试 3：B 移动到远处（跨 AOI 格）→ A 收到 disappear；B 移回 → A 收到 appear
bool Test_MoveCrossGridBroadcast() {
    g_test_failures = 0;
    std::cout << "\n[Test 3] MoveCrossGridBroadcast" << std::endl;
    const int port = 21003;

    TestServer server;
    server.Start(port);

    TestClient a("127.0.0.1", port, "user_A3");
    TestClient b("127.0.0.1", port, "user_B3");
    a.Start();
    b.Start();

    bool in_game = WaitForInGame(a) && WaitForInGame(b);
    PROTO_CHECK(in_game, "clients failed to enter game");

    if (in_game) {
        bool a_app = a.msg_queue()->WaitFor(proto_id("cli_3d_aoi_appears_ntf"), 5000);
        bool b_app = b.msg_queue()->WaitFor(proto_id("cli_3d_aoi_appears_ntf"), 5000);
        std::cout << "  [Phase1] A got appear=" << a_app
                  << "  B got appear=" << b_app << std::endl;
        PROTO_CHECK(a_app, "A did not receive appear from B");
        PROTO_CHECK(b_app, "B did not receive appear from A");

        {
            ::cli_3d_aoi_appears_ntf appear;
            if (TakeFirst(*a.msg_queue(), proto_id("cli_3d_aoi_appears_ntf"), &appear) && appear.list_size() > 0) {
                float px = 0, py = 0, pz = 0;
                ExtractPlayerPos(appear.list(0), &px, &py, &pz);
                std::cout << "  [A sees B] entity_id=" << appear.list(0).entity_id()
                          << " type=" << appear.list(0).type()
                          << " pos=(" << px << "," << py << "," << pz << ")" << std::endl;
            }
        }
        {
            ::cli_3d_aoi_appears_ntf appear;
            if (TakeFirst(*b.msg_queue(), proto_id("cli_3d_aoi_appears_ntf"), &appear) && appear.list_size() > 0) {
                float px = 0, py = 0, pz = 0;
                ExtractPlayerPos(appear.list(0), &px, &py, &pz);
                std::cout << "  [B sees A] entity_id=" << appear.list(0).entity_id()
                          << " type=" << appear.list(0).type()
                          << " pos=(" << px << "," << py << "," << pz << ")" << std::endl;
            }
        }

        a.msg_queue()->Drain();
        b.msg_queue()->Drain();

        b.SendMove(1000.0f, 20.0f, 1000.0f);
        PROTO_CHECK(b.WaitForMoveRes(5000), "B move_res timeout (far)");

        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        bool a_dis = a.msg_queue()->WaitFor(proto_id("cli_3d_aoi_disappears_ntf"), 5000);
        bool b_dis = b.msg_queue()->WaitFor(proto_id("cli_3d_aoi_disappears_ntf"), 5000);
        std::cout << "  [Phase2] A got disappear=" << a_dis
                  << "  B got disappear=" << b_dis << std::endl;
        PROTO_CHECK(a_dis, "A did not receive disappear after B moved far");
        PROTO_CHECK(b_dis, "B did not receive disappear after A moved away");

        {
            ::cli_3d_aoi_disappears_ntf dis;
            if (TakeFirst(*a.msg_queue(), proto_id("cli_3d_aoi_disappears_ntf"), &dis) && dis.entity_id_list_size() > 0) {
                std::cout << "  [A lost B] entity_id=" << dis.entity_id_list(0) << std::endl;
            }
        }
        {
            ::cli_3d_aoi_disappears_ntf dis;
            if (TakeFirst(*b.msg_queue(), proto_id("cli_3d_aoi_disappears_ntf"), &dis) && dis.entity_id_list_size() > 0) {
                std::cout << "  [B lost A] entity_id=" << dis.entity_id_list(0) << std::endl;
            }
        }

        a.msg_queue()->Drain();
        b.msg_queue()->Drain();

        b.SendMove(333.0f, 18.0f, 415.45f);
        PROTO_CHECK(b.WaitForMoveRes(5000), "B move_res timeout (back)");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        bool a_reapp = a.msg_queue()->WaitFor(proto_id("cli_3d_aoi_appears_ntf"), 8000);
        bool b_reapp = b.msg_queue()->WaitFor(proto_id("cli_3d_aoi_appears_ntf"), 8000);
        std::cout << "  [Phase3] A re-got appear=" << a_reapp
                  << "  B re-got appear=" << b_reapp << std::endl;
        PROTO_CHECK(a_reapp, "A did not receive appear after B moved back");
        PROTO_CHECK(b_reapp, "B did not receive appear after A moved back");

        if (a_reapp) {
            ::cli_3d_aoi_appears_ntf appear;
            if (TakeFirst(*a.msg_queue(), proto_id("cli_3d_aoi_appears_ntf"), &appear) && appear.list_size() > 0) {
                float px = 0, py = 0, pz = 0;
                ExtractPlayerPos(appear.list(0), &px, &py, &pz);
                std::cout << "  [A re-sees B] entity_id=" << appear.list(0).entity_id()
                          << " pos=(" << px << "," << py << "," << pz << ")" << std::endl;
            }
        }
    }

    a.Stop();
    b.Stop();
    server.Stop();
    return g_test_failures == 0;
}

// 测试 4：三玩家互相 appear 广播
bool Test_ThreePlayersMutualAppear() {
    g_test_failures = 0;
    std::cout << "\n[Test 4] ThreePlayersMutualAppear" << std::endl;
    const int port = 21004;

    TestServer server;
    server.Start(port);

    TestClient a("127.0.0.1", port, "user_A4");
    TestClient b("127.0.0.1", port, "user_B4");
    TestClient c("127.0.0.1", port, "user_C4");
    a.Start();
    b.Start();
    c.Start();

    bool in_game = WaitForInGame(a) && WaitForInGame(b) && WaitForInGame(c);
    PROTO_CHECK(in_game, "clients failed to enter game");

    if (in_game) {
        bool a_ok = a.msg_queue()->WaitForCount(proto_id("cli_3d_aoi_appears_ntf"), 2, 5000);
        bool b_ok = b.msg_queue()->WaitForCount(proto_id("cli_3d_aoi_appears_ntf"), 2, 5000);
        bool c_ok = c.msg_queue()->WaitForCount(proto_id("cli_3d_aoi_appears_ntf"), 2, 5000);
        PROTO_CHECK(a_ok, "A did not receive 2 appears");
        PROTO_CHECK(b_ok, "B did not receive 2 appears");
        PROTO_CHECK(c_ok, "C did not receive 2 appears");
    }

    a.Stop();
    b.Stop();
    c.Stop();
    server.Stop();
    return g_test_failures == 0;
}

// 测试 5：appear 帧字段完整性——entity_id/type/entity_data_list 非空
bool Test_AppearBodyFieldsComplete() {
    g_test_failures = 0;
    std::cout << "\n[Test 5] AppearBodyFieldsComplete" << std::endl;
    const int port = 21005;

    TestServer server;
    server.Start(port);

    TestClient a("127.0.0.1", port, "user_A5");
    TestClient b("127.0.0.1", port, "user_B5");
    a.Start();
    b.Start();

    bool in_game = WaitForInGame(a) && WaitForInGame(b);
    PROTO_CHECK(in_game, "clients failed to enter game");

    if (in_game) {
        bool got = a.msg_queue()->WaitFor(proto_id("cli_3d_aoi_appears_ntf"), 5000);
        PROTO_CHECK(got, "A did not receive appear");
        if (got) {
            ::cli_3d_aoi_appears_ntf appear;
            bool parsed = TakeFirst(*a.msg_queue(), proto_id("cli_3d_aoi_appears_ntf"), &appear);
            PROTO_CHECK(parsed, "failed to parse appears_ntf");
            if (parsed && appear.list_size() > 0) {
                const auto& e3d = appear.list(0);
                PROTO_CHECK(e3d.entity_id() != 0, "appear entity_id is 0");
                PROTO_CHECK(e3d.type() == ::ENTITY_PLAYER, "appear type != ENTITY_PLAYER");
                PROTO_CHECK(e3d.entity_data_list_size() > 0, "appear entity_data_list is empty");
            }
        }
    }

    a.Stop();
    b.Stop();
    server.Stop();
    return g_test_failures == 0;
}

// 测试 6：enter_map 帧字段——role_entity_id 非零、err_code==0
bool Test_EnterMapBodyFields() {
    g_test_failures = 0;
    std::cout << "\n[Test 6] EnterMapBodyFields" << std::endl;
    const int port = 21006;

    TestServer server;
    server.Start(port);

    TestClient a("127.0.0.1", port, "user_A6");
    a.Start();

    bool in_game = WaitForInGame(a);
    PROTO_CHECK(in_game, "client failed to enter game");

    if (in_game) {
        bool got = a.msg_queue()->WaitFor(proto_id("cli_3d_enter_map_ntf"), 5000);
        PROTO_CHECK(got, "A did not receive enter_map_ntf");
        if (got) {
            ::cli_3d_enter_map_ntf em;
            bool parsed = TakeFirst(*a.msg_queue(), proto_id("cli_3d_enter_map_ntf"), &em);
            PROTO_CHECK(parsed, "failed to parse enter_map_ntf");
            if (parsed) {
                PROTO_CHECK(em.err_code() == 0, "enter_map err_code != 0");
                PROTO_CHECK(em.role_entity_id() != 0, "enter_map role_entity_id is 0");
            }
        }
    }

    a.Stop();
    server.Stop();
    return g_test_failures == 0;
}

// 测试 7：B 进图后 A 收到 appear，B 离线后 A 收到 disappear，
//        B 重连重新进图后 A 再次收到 appear（完整生命周期）
bool Test_ReconnectAppearAgain() {
    g_test_failures = 0;
    std::cout << "\n[Test 7] ReconnectAppearAgain" << std::endl;
    const int port = 21007;

    TestServer server;
    server.Start(port);

    TestClient a("127.0.0.1", port, "user_A7");
    TestClient b("127.0.0.1", port, "user_B7");
    a.Start();
    b.Start();

    bool in_game = WaitForInGame(a) && WaitForInGame(b);
    PROTO_CHECK(in_game, "clients failed to enter game");

    if (in_game) {
        a.msg_queue()->WaitFor(proto_id("cli_3d_aoi_appears_ntf"), 5000);
        b.msg_queue()->WaitFor(proto_id("cli_3d_aoi_appears_ntf"), 5000);

        a.msg_queue()->Drain();
        b.Stop();
        bool got_dis = a.msg_queue()->WaitFor(proto_id("cli_3d_aoi_disappears_ntf"), 5000);
        std::cout << "  A got disappear=" << got_dis << std::endl;
        PROTO_CHECK(got_dis, "A did not receive disappear after B left");

        if (got_dis) {
            a.msg_queue()->Drain();
            TestClient b2("127.0.0.1", port, "user_B7");
            b2.Start();
            bool b2_in = WaitForInGame(b2);
            PROTO_CHECK(b2_in, "B2 failed to enter game");
            if (b2_in) {
                bool got_app = a.msg_queue()->WaitFor(proto_id("cli_3d_aoi_appears_ntf"), 5000);
                std::cout << "  A re-got appear=" << got_app << std::endl;
                PROTO_CHECK(got_app, "A did not receive appear after B2 entered");
            }
            b2.Stop();
        }
    }

    a.Stop();
    server.Stop();
    return g_test_failures == 0;
}

// 测试 8：负坐标大范围移动 — A 在出生点附近，B 移到 (-500,-500) 双方互相 disappear
bool Test_MoveNegativeCoordsAoiBroadcast() {
    g_test_failures = 0;
    std::cout << "\n[Test 8] MoveNegativeCoordsAoiBroadcast" << std::endl;
    const int port = 21008;

    TestServer server;
    server.Start(port);

    TestClient a("127.0.0.1", port, "user_A8");
    TestClient b("127.0.0.1", port, "user_B8");
    a.Start();
    b.Start();

    bool in_game = WaitForInGame(a) && WaitForInGame(b);
    PROTO_CHECK(in_game, "clients failed to enter game");

    if (in_game) {
        a.msg_queue()->WaitFor(proto_id("cli_3d_aoi_appears_ntf"), 5000);
        b.msg_queue()->WaitFor(proto_id("cli_3d_aoi_appears_ntf"), 5000);
        a.msg_queue()->Drain();
        b.msg_queue()->Drain();

        b.SendMove(-500.0f, 20.0f, -500.0f);
        PROTO_CHECK(b.WaitForMoveRes(5000), "B move_res timeout (negative far)");
        bool a_dis = a.msg_queue()->WaitFor(proto_id("cli_3d_aoi_disappears_ntf"), 5000);
        bool b_dis = b.msg_queue()->WaitFor(proto_id("cli_3d_aoi_disappears_ntf"), 5000);
        PROTO_CHECK(a_dis, "A did not receive disappear after B moved to negative coords");
        PROTO_CHECK(b_dis, "B did not receive disappear after moving away from A");

        if (a_dis && b_dis) {
            a.msg_queue()->Drain();
            b.msg_queue()->Drain();
            b.SendMove(333.0f, 18.0f, 415.45f);
            PROTO_CHECK(b.WaitForMoveRes(5000), "B move_res timeout (back)");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            bool a_app = a.msg_queue()->WaitFor(proto_id("cli_3d_aoi_appears_ntf"), 8000);
            PROTO_CHECK(a_app, "A did not receive appear after B moved back to spawn");
        }
    }

    a.Stop();
    b.Stop();
    server.Stop();
    return g_test_failures == 0;
}

}  // namespace

int main(int argc, char** argv) {
    InitSignals();
#ifdef _WIN32
    WSADATA wsa_data{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) return 1;
#endif

    std::cout << "=== svc_game_3d_protocol_test ===" << std::endl;
    std::cout << "Testing real protocol flow over loopback TCP:" << std::endl;
    std::cout << "  handshake -> login -> role -> enter_game -> AOI broadcast" << std::endl;

    struct TestCase {
        const char* name;
        bool (*fn)();
    };
    TestCase tests[] = {
        {"EnterMapAppearBroadcast",     Test_EnterMapAppearBroadcast},
        {"LeaveMapDisappearBroadcast",  Test_LeaveMapDisappearBroadcast},
        {"MoveCrossGridBroadcast",      Test_MoveCrossGridBroadcast},
        {"ThreePlayersMutualAppear",    Test_ThreePlayersMutualAppear},
        {"AppearBodyFieldsComplete",    Test_AppearBodyFieldsComplete},
        {"EnterMapBodyFields",          Test_EnterMapBodyFields},
        {"ReconnectAppearAgain",        Test_ReconnectAppearAgain},
        {"MoveNegativeCoordsAoiBroadcast", Test_MoveNegativeCoordsAoiBroadcast},
    };

    int passed = 0;
    int total = static_cast<int>(sizeof(tests) / sizeof(tests[0]));
    int run_only = -1;
    if (argc > 1) {
        run_only = std::atoi(argv[1]);
    }
    for (int i = 0; i < total; ++i) {
        if (run_only >= 0 && i != run_only) continue;
        bool ok = tests[i].fn();
        std::cout << (ok ? "[  OK  ] " : "[ FAIL ] ") << tests[i].name << std::endl;
        if (ok) ++passed;
    }

    std::cout << "\n=== Protocol test: " << passed << "/" << total << " passed ==="
              << std::endl;

#ifdef _WIN32
    WSACleanup();
#endif
    return (passed == total) ? 0 : 1;
}
