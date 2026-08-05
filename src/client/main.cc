// client/main.cc - svc_game_3d_client
// 完整协议对接测试客户端：handshake → login → role_list → role_login → enter_game
// → move → heart_beat（持续心跳，不退程序）

#include <google/protobuf/message.h>

#include <any>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "client_3d.pb.h"
#include "client_login.pb.h"
#include "protocol/pack_codec.h"
#include "protocol/pack_flags.h"
#include "common/init.h"
#include "zrpc/base/buffer.h"
#include "zrpc/base/logger.h"
#include "zrpc/net/event_loop.h"
#include "zrpc/net/tcp_client.h"
#include "zrpc/net/tcp_connection.h"

#ifdef _WIN32
#include <winsock2.h>
#endif

using namespace zrpc;

enum class ClientPhase : uint8_t
{
    kIdle,              // 初始
    kHandshakeSent,     // 已发握手
    kLoggedIn,          // 已登录
    kRoleListReceived,  // 已收到角色列表
    kRoleLoginSent,     // 已发角色登录
    kInGame,            // 已进入游戏（持续在线，定时心跳）
    kMoveSent,          // 已发移动
};

struct ClientState
{
    uint8_t  send_seq        = 0;
    uint64_t session_id      = 0;
    uint64_t role_id         = 0;
    ClientPhase phase        = ClientPhase::kIdle;
    int      move_count      = 0;     // 已发送的移动次数
    int      max_moves       = 3;     // 最大移动次数
    int      heartbeat_count = 0;     // 累计心跳次数
    int      delay_ms        = 500;   // 每步间隔（模拟客户端节奏）
};

void SendFrame(const std::shared_ptr<TcpConnection>& conn, uint32_t msg_id,
               const google::protobuf::Message& msg, ClientState* st)
{
    PackFrame frame;
    frame.flags      = kPackFlagEncrypt;
    frame.msg_id     = msg_id;
    frame.recv_index = st->send_seq++;
    if (!msg.SerializeToString(&frame.body))
    {
        LOG_WARN << "Serialize failed";
        return;
    }
    Buffer out;
    EncodeFrame(frame, &out);
    conn->Send(&out);
    LOG_INFO << ">>> [SEND] " << msg_id_name(msg_id) << " msg_id=" << msg_id
             << " body=" << frame.body.size() << "B"
             << " seq=" << (uint32_t)frame.recv_index;
}

void SendHandshake(const std::shared_ptr<TcpConnection>& conn, ClientState* st)
{
    ::cli_handshake_req req;
    req.set_version("svc_game_3d_client/1.0");
    SendFrame(conn, proto_id("cli_handshake_req"), req, st);
    st->phase = ClientPhase::kHandshakeSent;
    LOG_INFO << ">>> [SEND] cli_handshake_req";
}

void SendUserLogin(const std::shared_ptr<TcpConnection>& conn, ClientState* st,
                   const std::string& uid, const std::string& token)
{
    ::cli_user_login_req req;
    req.set_uid(uid);
    req.set_token(token);
    req.set_channel_id(1);
    SendFrame(conn, proto_id("cli_user_login_req"), req, st);
    st->phase = ClientPhase::kLoggedIn;
    LOG_INFO << ">>> [SEND] cli_user_login_req uid=" << uid;
}

void SendRoleList(const std::shared_ptr<TcpConnection>& conn, ClientState* st)
{
    ::cli_role_list_req req;
    SendFrame(conn, proto_id("cli_role_list_req"), req, st);
    LOG_INFO << ">>> [SEND] cli_role_list_req";
}

void SendRoleCreate(const std::shared_ptr<TcpConnection>& conn, ClientState* st,
                    const std::string& name)
{
    ::cli_role_create_req req;
    auto* info = req.mutable_role_info();
    info->set_name(name);
    info->set_sex(1);
    info->set_job(1);
    SendFrame(conn, proto_id("cli_role_create_req"), req, st);
    LOG_INFO << ">>> [SEND] cli_role_create_req name=" << name;
}

void SendRoleLogin(const std::shared_ptr<TcpConnection>& conn, ClientState* st,
                   uint64_t role_id)
{
    ::cli_role_login_req req;
    req.set_role_id(role_id);
    req.set_op_code(1);
    SendFrame(conn, proto_id("cli_role_login_req"), req, st);
    st->phase = ClientPhase::kRoleLoginSent;
    LOG_INFO << ">>> [SEND] cli_role_login_req role_id=" << role_id;
}

void SendEnterGame(const std::shared_ptr<TcpConnection>& conn, ClientState* st)
{
    ::cli_enter_game_req req;
    SendFrame(conn, proto_id("cli_enter_game_req"), req, st);
    LOG_INFO << ">>> [SEND] cli_enter_game_req";
}

void SendMove(const std::shared_ptr<TcpConnection>& conn, ClientState* st,
              float x, float y, float z)
{
    ::cli_3d_move_req req;
    auto* m = req.mutable_move();
    auto* pos = m->mutable_pos();
    pos->set_x(x);
    pos->set_y(y);
    pos->set_z(z);
    auto* rot = m->mutable_rot();
    rot->set_x(0);
    rot->set_y(0);
    rot->set_z(0);
    rot->set_w(1);
    auto* vel = m->mutable_velocity();
    vel->set_x(1.0f);
    vel->set_y(0);
    vel->set_z(0);
    auto* mrot = m->mutable_move_rot();
    mrot->set_x(0);
    mrot->set_y(0);
    mrot->set_z(0);
    mrot->set_w(1);
    m->set_status(0);
    SendFrame(conn, proto_id("cli_3d_move_req"), req, st);
    st->phase = ClientPhase::kMoveSent;
    LOG_INFO << ">>> [SEND] cli_3d_move_req pos=(" << x << "," << y << "," << z << ")";
}

void SendHeartbeat(const std::shared_ptr<TcpConnection>& conn, ClientState* st)
{
    ::cli_heart_beat_req req;
    SendFrame(conn, proto_id("cli_heart_beat_req"), req, st);
    LOG_INFO << ">>> [SEND] cli_heart_beat_req";
}

// 占位：回调内不能 sleep，否则会卡死事件循环
void DelayMs(int /*ms*/) {}

void HandleFrames(const std::shared_ptr<TcpConnection>& conn,
                  const std::vector<PackFrame>& frames, ClientState* st)
{
    for (const PackFrame& frame : frames)
    {
        if (frame.msg_id == proto_id("cli_handshake_res"))
        {
            ::cli_handshake_res rsp;
            if (!rsp.ParseFromString(frame.body))
            {
                LOG_WARN << "<<< [RECV] bad cli_handshake_res";
                break;
            }
            LOG_INFO << "<<< [RECV] " << msg_id_name(frame.msg_id)
                     << " err_code=" << rsp.err_code()
                     << " msg=" << rsp.msg();
            if (rsp.err_code() == 0)
            {
                DelayMs(st->delay_ms);
                SendUserLogin(conn, st, "test_user_001", "token_abc");
            }
        }
        else if (frame.msg_id == proto_id("cli_user_login_res"))
        {
            ::cli_user_login_res rsp;
            if (!rsp.ParseFromString(frame.body))
            {
                LOG_WARN << "bad cli_user_login_res";
                break;
            }
            LOG_INFO << "<<< [RECV] " << msg_id_name(frame.msg_id)
                     << " err_code=" << rsp.err_code()
                     << " session_id=" << rsp.session_id()
                     << " gate_addr=" << rsp.gate_addr();
            if (rsp.err_code() == 0)
            {
                st->session_id = rsp.session_id();
                DelayMs(st->delay_ms);
                SendRoleList(conn, st);
            }
        }
        else if (frame.msg_id == proto_id("cli_role_list_res"))
        {
            ::cli_role_list_res rsp;
            if (!rsp.ParseFromString(frame.body))
            {
                LOG_WARN << "bad cli_role_list_res";
                break;
            }
            LOG_INFO << "<<< [RECV] " << msg_id_name(frame.msg_id)
                     << " err_code=" << rsp.err_code()
                     << " op_code=" << rsp.op_code()
                     << " role_count=" << rsp.role_list_size();
            for (int i = 0; i < rsp.role_list_size(); ++i)
            {
                const auto& r = rsp.role_list(i);
                LOG_INFO << "    role[" << i << "] id=" << r.role_id()
                         << " name=" << r.name()
                         << " sex=" << r.sex()
                         << " job=" << r.job()
                         << " create_time=" << r.create_time()
                         << " is_last=" << r.last();
            }

            if (rsp.err_code() != 0)
            {
                LOG_WARN << "role_list err, stop";
                conn->Shutdown();
                break;
            }

            // 如果有角色，直接登录第一个；否则创建新角色
            DelayMs(st->delay_ms);
            if (rsp.role_list_size() > 0)
            {
                st->role_id = rsp.role_list(0).role_id();
                SendRoleLogin(conn, st, st->role_id);
            }
            else
            {
                SendRoleCreate(conn, st, "TestPlayer");
            }
        }
        else if (frame.msg_id == proto_id("cli_role_login_res"))
        {
            ::cli_role_login_res rsp;
            if (!rsp.ParseFromString(frame.body))
            {
                LOG_WARN << "bad cli_role_login_res";
                break;
            }
            LOG_INFO << "<<< [RECV] " << msg_id_name(frame.msg_id)
                     << " err_code=" << rsp.err_code()
                     << " role_id=" << rsp.role_id()
                     << " op_code=" << rsp.op_code();
            if (rsp.err_code() == 0)
            {
                st->phase = ClientPhase::kRoleListReceived;
                DelayMs(st->delay_ms);
                SendEnterGame(conn, st);
            }
        }
        else if (frame.msg_id == proto_id("cli_enter_game_res"))
        {
            ::cli_enter_game_res rsp;
            if (!rsp.ParseFromString(frame.body))
            {
                LOG_WARN << "bad cli_enter_game_res";
                break;
            }
            LOG_INFO << "<<< [RECV] " << msg_id_name(frame.msg_id)
                     << " err_code=" << rsp.err_code();
        }
        else if (frame.msg_id == proto_id("cli_global_config_ntf"))
        {
            ::cli_global_config_ntf rsp;
            if (!rsp.ParseFromString(frame.body))
            {
                LOG_WARN << "bad cli_global_config_ntf";
                break;
            }
            LOG_INFO << "<<< [RECV] " << msg_id_name(frame.msg_id)
                     << " (size=" << frame.body.size() << ")";
        }
        else if (frame.msg_id == proto_id("cli_3d_enter_map_ntf"))
        {
            ::cli_3d_enter_map_ntf rsp;
            if (!rsp.ParseFromString(frame.body))
            {
                LOG_WARN << "bad cli_3d_enter_map_ntf";
                break;
            }
            LOG_INFO << "<<< [RECV] " << msg_id_name(frame.msg_id)
                     << " cfg_id=" << rsp.cfg_id()
                     << " map_id=" << rsp.map_id()
                     << " source_id=" << rsp.source_id()
                     << " role_entity_id=" << rsp.role_entity_id()
                     << " err_code=" << rsp.err_code();
            LOG_INFO << "    born_pos=(" << rsp.pos().x()
                     << "," << rsp.pos().y()
                     << "," << rsp.pos().z() << ")";

            if (rsp.err_code() == 0)
            {
                st->phase = ClientPhase::kInGame;
                DelayMs(st->delay_ms);
                SendMove(conn, st, 100.0f, 20.0f, 200.0f);
            }
        }
        else if (frame.msg_id == proto_id("cli_3d_aoi_appears_ntf"))
        {
            ::cli_3d_aoi_appears_ntf rsp;
            if (!rsp.ParseFromString(frame.body))
            {
                LOG_WARN << "bad cli_3d_aoi_appears_ntf";
                break;
            }
            LOG_INFO << "<<< [RECV] " << msg_id_name(frame.msg_id)
                     << " count=" << rsp.list_size();
            for (const auto& e3d : rsp.list()) {
                LOG_INFO << "    entity_id=" << e3d.entity_id()
                         << " type=" << e3d.type()
                         << " data_count=" << e3d.entity_data_list().size();
            }
        }
        else if (frame.msg_id == proto_id("cli_3d_aoi_disappears_ntf"))
        {
            ::cli_3d_aoi_disappears_ntf rsp;
            if (!rsp.ParseFromString(frame.body))
            {
                LOG_WARN << "bad cli_3d_aoi_disappears_ntf";
                break;
            }
            LOG_INFO << "<<< [RECV] " << msg_id_name(frame.msg_id)
                     << " count=" << rsp.entity_id_list_size();
        }
        else if (frame.msg_id == proto_id("cli_3d_move_res"))
        {
            ::cli_3d_move_res rsp;
            if (!rsp.ParseFromString(frame.body))
            {
                LOG_WARN << "bad cli_3d_move_res";
                break;
            }
            LOG_INFO << "<<< [RECV] " << msg_id_name(frame.msg_id)
                     << " entity_id=" << rsp.entity_id()
                     << " err_code=" << rsp.err_code()
                     << " pos=(" << rsp.move().pos().x() << "," << rsp.move().pos().y() << "," << rsp.move().pos().z() << ")"
                     << " status=" << rsp.move().status();

            st->move_count++;
            if (st->move_count < st->max_moves)
            {
                float nx = 100.0f + st->move_count * 10.0f;
                float nz = 200.0f + st->move_count * 10.0f;
                SendMove(conn, st, nx, 20.0f, nz);
            }
            break;
        }

        else if (frame.msg_id == proto_id("cli_heart_beat_res"))
        {
            // 服务端心跳回包，忽略
        }
        else
        {
            LOG_WARN << "<<< [RECV] " << msg_id_name(frame.msg_id)
                     << " unexpected msg_id=" << frame.msg_id;
        }
    }
}

class GameClient
{
public:
    GameClient(std::string ip, int port)
        : client_(&loop_, std::move(ip), port, nullptr) {}

    void Start()
    {
        client_.SetConnectionCallback(
            std::bind(&GameClient::OnConnection, this, std::placeholders::_1));
        client_.SetMessageCallback(
            std::bind(&GameClient::OnMessage, this, std::placeholders::_1,
                      std::placeholders::_2));
        std::cout << "connecting to server..." << std::endl;
        client_.Connect();
    }

    void Loop() { loop_.Run(); }

private:
    void OnConnection(const std::shared_ptr<TcpConnection>& conn)
    {
        if (conn->Connected())
        {
            socket::SetKeepAlive(conn->GetSockfd(), 1);
            ClientState st;
            std::cout << "========================================" << std::endl;
            std::cout << "connected to server, start protocol test" << std::endl;
            std::cout << "========================================" << std::endl;
            SendHandshake(conn, &st);
            conn->SetContext(st);

            // 连接成功后立即启动心跳循环定时器
            StartHeartbeatLoop(conn);
        }
        else
        {
            std::cout << "========================================" << std::endl;
            std::cout << "disconnected" << std::endl;
            std::cout << "========================================" << std::endl;
        }
    }

    void StartHeartbeatLoop(const std::shared_ptr<TcpConnection>& conn)
    {
        std::weak_ptr<TcpConnection> weak_conn = conn;
        loop_.RunAfter(2.0, false, [this, weak_conn]()
        {
            auto conn = weak_conn.lock();
            if (!conn || !conn->Connected())
            {
                return;
            }
            ClientState cur_st = std::any_cast<ClientState>(conn->GetContext());
            cur_st.heartbeat_count++;
            SendHeartbeat(conn, &cur_st);
            std::cout << ">>> heartbeat #" << cur_st.heartbeat_count << std::endl;
            conn->SetContext(cur_st);

            // 继续下一次心跳
            StartHeartbeatLoop(conn);
        });
    }

    void OnMessage(const std::shared_ptr<TcpConnection>& conn, Buffer* buf)
    {
        std::vector<PackFrame> frames;
        if (!TryDecodeFrames(buf, frames))
        {
            LOG_WARN << "protocol error";
            conn->Shutdown();
            return;
        }
        ClientState st = std::any_cast<ClientState>(conn->GetContext());
        HandleFrames(conn, frames, &st);
        conn->SetContext(st);
    }

    EventLoop  loop_;
    TcpClient  client_;
};

int main(int argc, char** argv)
{
    InitSignals();

#ifdef _WIN32
    WSADATA wsa_data{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0)
    {
        return 1;
    }
#endif

    std::string ip = "127.0.0.1";
    int port = 20002;  // 与 svc_game_3d_server 默认端口一致
    if (argc >= 2) ip   = argv[1];
    if (argc >= 3) port = std::stoi(argv[2]);

    std::cout << "svc_game_3d_client -> " << ip << ":" << port << std::endl;
    std::cout << "protocol test flow: handshake -> login -> role_list -> role_login"
              << " -> enter_game -> move x3, heartbeat (every 2s, never exit)"
              << std::endl;

    GameClient client(ip, port);
    client.Start();
    client.Loop();

#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
