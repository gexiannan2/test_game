// client/stress_test.cc - 连外部 svc_game_3d_server 的多客户端压测
//
// 先启服再压测（务必 ulimit -n 65535 再跑 5000）:
//   ./bin/svc_game_3d_server 20002 127.0.0.1
//   ./bin/svc_game_3d_stress_test 5000 127.0.0.1 20002 mixed
//
// 模式:
//   normal   登录进图→有限移动→心跳常驻
//   churn    进图后随机断线重连（服务端 LeaveMap + AOI disappear）
//   kickoff  同 uid 池反复登录顶号
//   move     进图后持续移动 + 心跳（AOI 跨格 appear/disappear）
//   mixed    churn + move（推荐做 5000 人综合验证）
//   badmove  进图后逐个发送越界/NaN/Inf 坐标，验证服务端钳制与拒绝响应
//
// 用法: svc_game_3d_stress_test <clients> <ip> <port> <mode> [uid_prefix] [uid_count]
// 客户端分批从 0 爬到 N；每 2 秒打印业务统计 + 各 msg_id 收包计数。

#include <google/protobuf/message.h>

#include <algorithm>
#include <any>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
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
#include "zrpc/net/tcp_client.h"
#include "zrpc/net/tcp_connection.h"

#ifdef _WIN32
#include <winsock2.h>
#endif

using namespace zrpc;

// 统计（原子计数，每 2 秒打印快照 + 增量）
struct StressStats {
  // 连接/会话
  std::atomic<int> connect_attempts{0};
  std::atomic<int> connected{0};
  std::atomic<int> disconnected{0};
  std::atomic<int> reconnects{0};

  // 协议流程
  std::atomic<int> handshakes_ok{0};
  std::atomic<int> logged_in{0};
  std::atomic<int> role_list_received{0};
  std::atomic<int> role_created{0};
  std::atomic<int> role_login_ok{0};
  std::atomic<int> entered_game{0};       // cli_3d_enter_map_ntf err_code==0

  // AOI 广播
  std::atomic<int> appear_received{0};      // cli_3d_aoi_appears_ntf
  std::atomic<int> disappears_batch_received{0}; // cli_3d_aoi_disappears_ntf
  std::atomic<int> enter_map_received{0};   // cli_3d_enter_map_ntf

  // 移动
  std::atomic<int> move_sent{0};
  std::atomic<int> move_res_received{0};    // cli_3d_move_res

  // 心跳/顶号
  std::atomic<int> heartbeat_sent{0};
  std::atomic<int> heartbeat_res_received{0};
  std::atomic<int> kickoff_count{0};        // 被顶号次数

  // 异常
  std::atomic<int> proto_decode_errors{0};  // 协议解码失败
  std::atomic<int> login_errors{0};         // 登录失败
  std::atomic<int> role_login_errors{0};    // 角色登录失败
  std::atomic<int> unexpected_msgs{0};      // 未预期消息

  // 按真实 msg_id 收包计数
  mutable std::mutex recv_mu;
  std::unordered_map<uint32_t, int> recv_by_msg_id;

  void NoteRecv(uint32_t msg_id) {
    std::lock_guard<std::mutex> lk(recv_mu);
    ++recv_by_msg_id[msg_id];
  }

  // 当前在线（实时）
  int Online() const { return connected.load() - disconnected.load(); }

  // 打印快照：与上次快照对比，显示增量
  void PrintSnapshot(const StressStats& prev) const {
    auto delta = [](const std::atomic<int>& cur, const std::atomic<int>& p) {
      return cur.load() - p.load();
    };
    std::cout << "\n===== STRESS STATS (delta since last) ====="
              << "\n  [online]         " << Online()
              << "\n  [connect]        attempts=" << connect_attempts
              << " (+" << delta(connect_attempts, prev.connect_attempts) << ")"
              << "  connected=" << connected
              << " (+" << delta(connected, prev.connected) << ")"
              << "  disconnected=" << disconnected
              << " (+" << delta(disconnected, prev.disconnected) << ")"
              << "  reconnects=" << reconnects
              << " (+" << delta(reconnects, prev.reconnects) << ")"
              << "\n  [handshake]      ok=" << handshakes_ok
              << " (+" << delta(handshakes_ok, prev.handshakes_ok) << ")"
              << "\n  [login]          ok=" << logged_in
              << " (+" << delta(logged_in, prev.logged_in) << ")"
              << "  fail=" << login_errors
              << " (+" << delta(login_errors, prev.login_errors) << ")"
              << "\n  [role]           list=" << role_list_received
              << " (+" << delta(role_list_received, prev.role_list_received) << ")"
              << "  created=" << role_created
              << " (+" << delta(role_created, prev.role_created) << ")"
              << "  login_ok=" << role_login_ok
              << " (+" << delta(role_login_ok, prev.role_login_ok) << ")"
              << "  fail=" << role_login_errors
              << " (+" << delta(role_login_errors, prev.role_login_errors) << ")"
              << "\n  [enter_game]     entered=" << entered_game
              << " (+" << delta(entered_game, prev.entered_game) << ")"
              << "  enter_map_recv=" << enter_map_received
              << " (+" << delta(enter_map_received, prev.enter_map_received) << ")"
              << "\n  [AOI appear]     " << appear_received
              << " (+" << delta(appear_received, prev.appear_received) << ")"
              << "\n  [AOI disappear]  " << disappears_batch_received
              << " (+" << delta(disappears_batch_received, prev.disappears_batch_received) << ")"
              << "\n  [move]           sent=" << move_sent
              << " (+" << delta(move_sent, prev.move_sent) << ")"
              << "  res=" << move_res_received
              << " (+" << delta(move_res_received, prev.move_res_received) << ")"
              << "\n  [heartbeat]      sent=" << heartbeat_sent
              << " (+" << delta(heartbeat_sent, prev.heartbeat_sent) << ")"
              << "  res=" << heartbeat_res_received
              << " (+" << delta(heartbeat_res_received, prev.heartbeat_res_received) << ")"
              << "\n  [kickoff]        " << kickoff_count
              << " (+" << delta(kickoff_count, prev.kickoff_count) << ")"
              << "\n  [errors]         proto=" << proto_decode_errors
              << " (+" << delta(proto_decode_errors, prev.proto_decode_errors) << ")"
              << "  unexpected=" << unexpected_msgs
              << " (+" << delta(unexpected_msgs, prev.unexpected_msgs) << ")";

    std::cout << "\n==========================================\n" << std::endl;
  }

  // 快照到 prev（逐字段 load，atomic 不可拷贝）
  void SnapshotTo(StressStats* dst) const {
    dst->connect_attempts = connect_attempts.load();
    dst->connected = connected.load();
    dst->disconnected = disconnected.load();
    dst->reconnects = reconnects.load();
    dst->handshakes_ok = handshakes_ok.load();
    dst->logged_in = logged_in.load();
    dst->role_list_received = role_list_received.load();
    dst->role_created = role_created.load();
    dst->role_login_ok = role_login_ok.load();
    dst->entered_game = entered_game.load();
    dst->appear_received = appear_received.load();
    dst->disappears_batch_received = disappears_batch_received.load();
    dst->enter_map_received = enter_map_received.load();
    dst->move_sent = move_sent.load();
    dst->move_res_received = move_res_received.load();
    dst->heartbeat_sent = heartbeat_sent.load();
    dst->heartbeat_res_received = heartbeat_res_received.load();
    dst->kickoff_count = kickoff_count.load();
    dst->proto_decode_errors = proto_decode_errors.load();
    dst->login_errors = login_errors.load();
    dst->role_login_errors = role_login_errors.load();
    dst->unexpected_msgs = unexpected_msgs.load();
    std::lock_guard<std::mutex> lk(recv_mu);
    dst->recv_by_msg_id = recv_by_msg_id;
  }
};
StressStats g_stats;
StressStats g_prev_stats;

std::mt19937& GetRng() {
  thread_local std::mt19937 rng(std::random_device{}());
  return rng;
}
int RandInt(int min, int max) {
  std::uniform_int_distribution<int> dist(min, max);
  return dist(GetRng());
}
float RandFloat(float min, float max) {
  std::uniform_real_distribution<float> dist(min, max);
  return dist(GetRng());
}

static bool g_churn_mode   = false;
static bool g_kickoff_mode = false;
static bool g_move_mode    = false;
static bool g_mixed_mode   = false;
static bool g_badmove_mode = false;
static int  g_uid_count    = 0;
static std::string g_uid_prefix = "stress_";
static int  g_move_interval_ms = 200;  // move 模式下移动间隔

enum class Phase : uint8_t {
  kIdle, kHandshakeSent, kLoggedIn, kInGame, kMoving,
};

struct ClientState {
  uint8_t  send_seq   = 0;
  uint64_t session_id = 0;
  uint64_t role_id    = 0;
  Phase    phase      = Phase::kIdle;
  int      client_id  = 0;
  int      move_sent  = 0;
  int      max_moves  = 3;
  int      hb_count   = 0;
  int      badmove_idx= -1;   // badmove 模式: 当前测试用例索引
  float    cur_x      = 0.0f;
  float    cur_z      = 0.0f;

  // uid: kickoff 模式下多个 client 共享同一个 uid 来触发顶号
  std::string uid;
};

void SendFrame(const std::shared_ptr<TcpConnection>& conn, uint32_t msg_id,
               const google::protobuf::Message& msg, ClientState* st) {
  PackFrame frame;
  frame.flags      = kPackFlagEncrypt;
  frame.msg_id     = msg_id;
  frame.recv_index = st->send_seq++;
  if (!msg.SerializeToString(&frame.body)) {
    g_stats.proto_decode_errors++;
    return;
  }
  Buffer out;
  EncodeFrame(frame, &out);
  conn->Send(&out);
}

void SendHandshake(const std::shared_ptr<TcpConnection>& conn, ClientState* st) {
  ::cli_handshake_req req;
  req.set_version("stress_test/1.0");
  SendFrame(conn, proto_id("cli_handshake_req"), req, st);
  st->phase = Phase::kHandshakeSent;
}

void SendLogin(const std::shared_ptr<TcpConnection>& conn, ClientState* st) {
  ::cli_user_login_req req;
  req.set_uid(st->uid);
  req.set_token("token_" + st->uid);
  req.set_channel_id(1);
  SendFrame(conn, proto_id("cli_user_login_req"), req, st);
}

void SendRoleList(const std::shared_ptr<TcpConnection>& conn, ClientState* st) {
  ::cli_role_list_req req;
  SendFrame(conn, proto_id("cli_role_list_req"), req, st);
}

void SendRoleCreate(const std::shared_ptr<TcpConnection>& conn, ClientState* st) {
  ::cli_role_create_req req;
  auto* ri = req.mutable_role_info();
  ri->set_name("Stress_" + st->uid);
  ri->set_sex(RandInt(1, 2));
  ri->set_job(RandInt(1, 3));
  SendFrame(conn, proto_id("cli_role_create_req"), req, st);
}

void SendRoleLogin(const std::shared_ptr<TcpConnection>& conn, ClientState* st) {
  ::cli_role_login_req req;
  req.set_role_id(st->role_id);
  req.set_op_code(1);
  SendFrame(conn, proto_id("cli_role_login_req"), req, st);
}

void SendEnterGame(const std::shared_ptr<TcpConnection>& conn, ClientState* st) {
  ::cli_enter_game_req req;
  SendFrame(conn, proto_id("cli_enter_game_req"), req, st);
}

// 随机移动：在出生点附近游走，便于 AOI 互见（新手村 born ≈ 333/18/415）
void SendMove(const std::shared_ptr<TcpConnection>& conn, ClientState* st) {
  ::cli_3d_move_req req;
  float dx = RandFloat(-8.0f, 8.0f);
  float dz = RandFloat(-8.0f, 8.0f);
  st->cur_x += dx;
  st->cur_z += dz;
  // 钳制在出生点附近约 ±80m，保证多数人在同一 AOI 粗格簇
  constexpr float kBornX = 333.0f;
  constexpr float kBornZ = 415.45f;
  if (st->cur_x < kBornX - 80.f) st->cur_x = kBornX - 80.f;
  if (st->cur_x > kBornX + 80.f) st->cur_x = kBornX + 80.f;
  if (st->cur_z < kBornZ - 80.f) st->cur_z = kBornZ - 80.f;
  if (st->cur_z > kBornZ + 80.f) st->cur_z = kBornZ + 80.f;

  auto* m = req.mutable_move();
  auto* pos = m->mutable_pos();
  pos->set_x(st->cur_x);
  pos->set_y(20.0f);
  pos->set_z(st->cur_z);
  auto* rot = m->mutable_rot();
  rot->set_x(0); rot->set_y(0); rot->set_z(0); rot->set_w(1);
  auto* vel = m->mutable_velocity();
  vel->set_x(dx > 0 ? 5.0f : -5.0f);
  vel->set_y(0); vel->set_z(dz > 0 ? 5.0f : -5.0f);
  auto* mrot = m->mutable_move_rot();
  mrot->set_x(0); mrot->set_y(0); mrot->set_z(0); mrot->set_w(1);
  m->set_status(0);
  SendFrame(conn, proto_id("cli_3d_move_req"), req, st);
  g_stats.move_sent++;
}

// ---- badmove 模式：发送越界/非法坐标，展示服务端钳制/拒绝响应 ----

struct BadMoveCase {
    const char* desc;
    float x, y, z;
    bool nan_rot;
};

static const BadMoveCase kBadMoveCases[] = {
    {"x_out_high",       99999.0f,  20.0f,   413.0f, false},
    {"x_out_low",       -99999.0f,  20.0f,   413.0f, false},
    {"z_out_high",        330.0f,   20.0f,  99999.0f, false},
    {"z_out_low",         330.0f,   20.0f, -99999.0f, false},
    {"y_underground",     330.0f, -9999.0f,   413.0f, false},
    {"y_sky",             330.0f,  9999.0f,   413.0f, false},
    {"nan_pos",    std::numeric_limits<float>::quiet_NaN(),
                   std::numeric_limits<float>::quiet_NaN(),
                   std::numeric_limits<float>::quiet_NaN(), false},
    {"inf_pos",    std::numeric_limits<float>::infinity(),
                   std::numeric_limits<float>::infinity(), 0.0f, false},
    {"nan_rot_valid_pos", 330.0f,   20.0f,   413.0f, true},
};
static constexpr int kBadMoveCaseCount =
    static_cast<int>(sizeof(kBadMoveCases) / sizeof(kBadMoveCases[0]));

static const char* ErrCodeName(int code) {
    switch (code) {
        case 0:  return "SUCCESS";
        case 1:  return "FAIL";
        case 2:  return "INVALID_PARAM";
        case 7:  return "STATE_INVALID";
        case 9:  return "NOT_IN_MAP";
        default: return "OTHER";
    }
}

void PrintMoveRes(const ::cli_3d_move_res& res, int case_idx) {
    const char* desc = (case_idx >= 0 && case_idx < kBadMoveCaseCount)
                       ? kBadMoveCases[case_idx].desc : "?";
    std::cout << "\n  [BADMOVE RES] case#" << case_idx << " (" << desc << ")"
              << "\n    err_code  = " << res.err_code()
              << " (" << ErrCodeName(res.err_code()) << ")"
              << "\n    entity_id = " << res.entity_id();
    if (res.has_move()) {
        const auto& m = res.move();
        if (m.has_pos()) {
            std::cout << "\n    move.pos  = (" << m.pos().x() << ","
                      << m.pos().y() << "," << m.pos().z() << ")";
        }
        std::cout << "\n    move.status = " << m.status();
        if (m.has_rot()) {
            std::cout << "\n    move.rot  = (" << m.rot().x() << ","
                      << m.rot().y() << "," << m.rot().z() << ","
                      << m.rot().w() << ")";
        }
        if (m.has_velocity()) {
            std::cout << "\n    move.vel  = (" << m.velocity().x() << ","
                      << m.velocity().y() << "," << m.velocity().z() << ")";
        }
    }
    std::cout << std::endl;
}

void SendBadMove(const std::shared_ptr<TcpConnection>& conn,
                 ClientState* st, int case_idx) {
    if (case_idx < 0 || case_idx >= kBadMoveCaseCount) return;
    const auto& tc = kBadMoveCases[case_idx];
    ::cli_3d_move_req req;
    auto* m = req.mutable_move();
    auto* pos = m->mutable_pos();
    pos->set_x(tc.x);
    pos->set_y(tc.y);
    pos->set_z(tc.z);
    if (tc.nan_rot) {
        float nan = std::numeric_limits<float>::quiet_NaN();
        auto* rot = m->mutable_rot();
        rot->set_x(nan); rot->set_y(0); rot->set_z(0); rot->set_w(1);
    } else {
        auto* rot = m->mutable_rot();
        rot->set_x(0); rot->set_y(0); rot->set_z(0); rot->set_w(1);
    }
    auto* mrot = m->mutable_move_rot();
    mrot->set_x(0); mrot->set_y(0); mrot->set_z(0); mrot->set_w(1);
    auto* vel = m->mutable_velocity();
    vel->set_x(0); vel->set_y(0); vel->set_z(0);
    m->set_status(0);

    std::cout << "\n  [BADMOVE REQ] case#" << case_idx << " (" << tc.desc << ")"
              << "\n    send pos  = (" << tc.x << "," << tc.y << "," << tc.z << ")"
              << (tc.nan_rot ? "  +NaN_rot" : "")
              << std::endl;

    SendFrame(conn, proto_id("cli_3d_move_req"), req, st);
    g_stats.move_sent++;
}

void SendHeartbeat(const std::shared_ptr<TcpConnection>& conn, ClientState* st) {
  ::cli_heart_beat_req req;
  SendFrame(conn, proto_id("cli_heart_beat_req"), req, st);
  g_stats.heartbeat_sent++;
}

void HandleFrame(const std::shared_ptr<TcpConnection>& conn, const PackFrame& frame,
                 ClientState* st) {
  g_stats.NoteRecv(frame.msg_id);
  if (frame.msg_id == proto_id("cli_handshake_res")) {
    g_stats.handshakes_ok++;
    SendLogin(conn, st);
    st->phase = Phase::kLoggedIn;
  } else if (frame.msg_id == proto_id("cli_user_login_res")) {
      ::cli_user_login_res rsp;
      if (!rsp.ParseFromString(frame.body)) {
        g_stats.proto_decode_errors++;
        return;
      }
      if (rsp.err_code() == 0) {
        g_stats.logged_in++;
        st->session_id = rsp.session_id();
        SendRoleList(conn, st);
      } else {
        g_stats.login_errors++;
        conn->Shutdown();
      }
  } else if (frame.msg_id == proto_id("cli_role_list_res")) {
      ::cli_role_list_res rsp;
      if (!rsp.ParseFromString(frame.body)) {
        g_stats.proto_decode_errors++;
        return;
      }
      g_stats.role_list_received++;
      if (rsp.role_list_size() > 0) {
        st->role_id = rsp.role_list(0).role_id();
        SendRoleLogin(conn, st);
      } else {
        g_stats.role_created++;
        SendRoleCreate(conn, st);
      }
  } else if (frame.msg_id == proto_id("cli_role_login_res")) {
      ::cli_role_login_res rsp;
      if (!rsp.ParseFromString(frame.body)) {
        g_stats.proto_decode_errors++;
        return;
      }
      if (rsp.err_code() == 0) {
        g_stats.role_login_ok++;
        st->role_id = rsp.role_id();
        SendEnterGame(conn, st);
      } else {
        g_stats.role_login_errors++;
        conn->Shutdown();
      }
  } else if (frame.msg_id == proto_id("cli_enter_game_res")) {
      // 进游戏响应（空体），等 cli_3d_enter_map
  } else if (frame.msg_id == proto_id("cli_global_config_ntf")) {
      // 进游戏附带下发，忽略即可
  } else if (frame.msg_id == proto_id("cli_3d_enter_map_ntf")) {
      g_stats.enter_map_received++;
      ::cli_3d_enter_map_ntf rsp;
      if (!rsp.ParseFromString(frame.body)) {
        g_stats.proto_decode_errors++;
        return;
      }
      if (rsp.err_code() == 0) {
        st->phase = Phase::kInGame;
        g_stats.entered_game++;
        if (rsp.has_pos()) {
          st->cur_x = rsp.pos().x();
          st->cur_z = rsp.pos().z();
        }
        if (g_badmove_mode) {
          st->badmove_idx = 0;
          std::cout << "\n===== BADMOVE TEST START"
                    << " (uid=" << st->uid << " role_id=" << st->role_id
                    << ") =====" << std::endl;
          SendBadMove(conn, st, 0);
        } else {
          SendMove(conn, st);
        }
        st->phase = Phase::kMoving;
        if (st->hb_count == 0) {
          st->hb_count = 1;
          SendHeartbeat(conn, st);
        }
      }
  } else if (frame.msg_id == proto_id("cli_3d_aoi_appears_ntf")) {
      // AOI 广播：实体出现
      g_stats.appear_received++;
  } else if (frame.msg_id == proto_id("cli_3d_aoi_disappears_ntf")) {
      // AOI 广播：实体消失
      g_stats.disappears_batch_received++;
  } else if (frame.msg_id == proto_id("cli_heart_beat_res")) {
      g_stats.heartbeat_res_received++;
  } else if (frame.msg_id == proto_id("cli_3d_move_res")) {
      g_stats.move_res_received++;
      st->move_sent++;
      if (g_badmove_mode) {
        ::cli_3d_move_res res;
        if (res.ParseFromString(frame.body)) {
          PrintMoveRes(res, st->badmove_idx);
        } else {
          std::cout << "\n  [BADMOVE RES] case#" << st->badmove_idx
                    << " parse failed!" << std::endl;
          g_stats.proto_decode_errors++;
        }
        st->badmove_idx++;
        if (st->badmove_idx < kBadMoveCaseCount) {
          SendBadMove(conn, st, st->badmove_idx);
        } else {
          std::cout << "\n===== BADMOVE TEST DONE =====\n" << std::endl;
        }
      } else if (st->move_sent < st->max_moves && !g_move_mode && !g_mixed_mode) {
        // normal 模式：有限次移动后停（等心跳）
        // move/mixed 模式：持续高频移动（由定时器驱动，不依赖 move_res）
        SendMove(conn, st);
      }
  } else if (frame.msg_id == proto_id("cli_kickoff_player_ntf")) {
      // 被顶号踢下线
      g_stats.kickoff_count++;
      conn->Shutdown();
  } else {
      g_stats.unexpected_msgs++;
  }
}

class StressClient {
 public:
  StressClient(EventLoop* loop, const std::string& ip, int port, int id)
      : client_(loop, ip, port, nullptr), id_(id), loop_(loop) {}

  void Start() {
    client_.SetConnectionCallback(
        [this](const std::shared_ptr<TcpConnection>& conn) { OnConnection(conn); });
    client_.SetMessageCallback(
        [this](const std::shared_ptr<TcpConnection>& conn, Buffer* buf) {
          OnMessage(conn, buf);
        });
    g_stats.connect_attempts++;
    client_.Connect();
  }

 private:
  void OnConnection(const std::shared_ptr<TcpConnection>& conn) {
    if (conn->Connected()) {
      g_stats.connected++;
      ClientState st;
      st.client_id = id_;

      // kickoff 模式: 共享 uid 池, 反复顶号
      if (g_kickoff_mode && g_uid_count > 0) {
        st.uid = g_uid_prefix + std::to_string(RandInt(0, g_uid_count - 1));
      } else {
        st.uid = g_uid_prefix + std::to_string(id_);
      }

      SendHandshake(conn, &st);
      conn->SetContext(st);

      // churn / mixed: 随机在线后主动断线
      if (g_churn_mode || g_mixed_mode) {
        int online_sec = RandInt(5, 30);
        loop_->RunAfter(online_sec, false,
                        [this, weak = std::weak_ptr<TcpConnection>(conn)]() {
                          auto c = weak.lock();
                          if (c && c->Connected()) c->Shutdown();
                        });
      }

      // move / mixed: 进图后持续高频移动
      if (g_move_mode || g_mixed_mode) {
        StartMoveLoop(conn);
      }
    } else {
      g_stats.disconnected++;
      int delay = (g_churn_mode || g_kickoff_mode || g_mixed_mode) ? RandInt(1, 5) : 2;
      g_stats.reconnects++;
      loop_->RunAfter(delay, false, [this]() {
        g_stats.connect_attempts++;
        client_.Connect();
      });
    }
  }

  void StartMoveLoop(const std::shared_ptr<TcpConnection>& conn) {
    std::weak_ptr<TcpConnection> weak_conn = conn;
    loop_->RunAfter(g_move_interval_ms / 1000.0, false, [this, weak_conn]() {
      auto c = weak_conn.lock();
      if (!c || !c->Connected()) return;
      try {
        ClientState st = std::any_cast<ClientState>(c->GetContext());
        // 只有进图后才持续移动
        if (st.phase == Phase::kInGame || st.phase == Phase::kMoving) {
          st.max_moves = 999999;  // move 模式不限次数
          SendMove(c, &st);
          st.phase = Phase::kMoving;
          c->SetContext(st);
        }
      } catch (const std::bad_any_cast&) {
        // context 未设置或类型不匹配，跳过（异常处理）
      }
      StartMoveLoop(c);
    });
  }

  void StartHeartbeatLoop(const std::shared_ptr<TcpConnection>& conn) {
    std::weak_ptr<TcpConnection> weak_conn = conn;
    loop_->RunAfter(2.0, false, [this, weak_conn]() {
      auto c = weak_conn.lock();
      if (!c || !c->Connected()) return;
      try {
        ClientState st = std::any_cast<ClientState>(c->GetContext());
        SendHeartbeat(c, &st);
        c->SetContext(st);
      } catch (const std::bad_any_cast&) {
        // 异常处理：context 未就绪，跳过
      }
      StartHeartbeatLoop(c);
    });
  }

  void OnMessage(const std::shared_ptr<TcpConnection>& conn, Buffer* buf) {
    std::vector<PackFrame> frames;
    if (!TryDecodeFrames(buf, frames)) {
      g_stats.proto_decode_errors++;
      return;
    }
    try {
      ClientState st = std::any_cast<ClientState>(conn->GetContext());
      for (const auto& frame : frames) HandleFrame(conn, frame, &st);

      // 进游戏后启动心跳循环 (仅一次；进图时已发首包)
      if ((st.phase == Phase::kMoving || st.phase == Phase::kInGame) &&
          st.hb_count == 1) {
        st.hb_count = 2;
        StartHeartbeatLoop(conn);
      }
      conn->SetContext(st);
    } catch (const std::bad_any_cast&) {
      g_stats.proto_decode_errors++;
    }
  }

  TcpClient   client_;
  int         id_;
  EventLoop*  loop_;
};

void PrintStats(EventLoop* loop) {
  g_stats.PrintSnapshot(g_prev_stats);
  g_stats.SnapshotTo(&g_prev_stats);
  loop->RunAfter(2, false, [loop]() { PrintStats(loop); });
}

int main(int argc, char** argv) {
  InitSignals();

#ifdef _WIN32
  WSADATA wsa_data{};
  WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif

  std::string ip       = "127.0.0.1";
  int port             = 20002;
  int clients          = 500;
  std::string mode     = "normal";

  if (argc >= 2) clients = std::stoi(argv[1]);
  if (argc >= 3) ip      = argv[2];
  if (argc >= 4) port    = std::stoi(argv[3]);
  if (argc >= 5) mode    = argv[4];
  if (argc >= 6) g_uid_prefix = argv[5];
  if (argc >= 7) g_uid_count  = std::stoi(argv[6]);

  g_churn_mode   = (mode == "churn");
  g_kickoff_mode = (mode == "kickoff");
  g_move_mode    = (mode == "move");
  g_mixed_mode   = (mode == "mixed");
  g_badmove_mode = (mode == "badmove");

  std::cout << "svc_game_3d_stress_test -> " << ip << ":" << port
            << "  clients=" << clients << "  mode=" << mode;
  if (g_kickoff_mode)
    std::cout << "  uid_count=" << g_uid_count;
  if (g_move_mode || g_mixed_mode)
    std::cout << "  move_interval=" << g_move_interval_ms << "ms";
  std::cout << std::endl;

  std::cout << "modes: normal=在线心跳 | churn=频繁上下线 | move=高频移动 | "
               "mixed=上下线+移动 | kickoff=顶号 | badmove=非法坐标测试"
            << std::endl;

  EventLoop loop;

  // 全局持有所有 client，避免 lambda 中析构
  std::vector<std::shared_ptr<StressClient>> client_list;
  client_list.reserve(clients);

  // 分批从 0 爬到 N，避免瞬间连接风暴（5000 人约数十秒爬满）
  int batch = 50;
  int delay_ms = 0;
  for (int i = 0; i < clients; ++i) {
    int idx = i;  // uid: stress_0 .. stress_(N-1)
    auto c = std::make_shared<StressClient>(&loop, ip, port, idx);
    client_list.push_back(c);
    loop.RunAfter(delay_ms / 1000.0, false, [c]() {
      c->Start();
    });
    if ((i + 1) % batch == 0) delay_ms += 100;  // 每 50 个加 100ms
  }

  // 2 秒后开始打印统计，之后每 2 秒
  loop.RunAfter(2, false, [&loop]() { PrintStats(&loop); });

  std::cout << "ramp 0.." << (clients - 1) << " launching (batch=" << batch
            << ", staggered), stats every 2s..." << std::endl;
  // 降噪：压测客户端默认 WARN，避免刷屏淹没统计
  Logger::SetLogLevel(Logger::WARN);
  loop.Run();

#ifdef _WIN32
  WSACleanup();
#endif
  return 0;
}
