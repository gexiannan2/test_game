// protocol_locomotion_test.cc — 跳跃/下落/下滑 与 move 协议隔离（真实网络 loopback）
// 编译目标：svc_game_3d_protocol_locomotion_test

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
#include "common/init.h"
#include "game_server.h"
#include "protocol/pack_codec.h"
#include "protocol/pack_flags.h"
#include "server_constants.h"
#include "zrpc/base/buffer.h"
#include "zrpc/base/logger.h"
#include "zrpc/net/event_loop.h"
#include "zrpc/net/socket.h"
#include "zrpc/net/tcp_client.h"
#include "zrpc/net/tcp_connection.h"

using namespace zrpc;

namespace {

int g_failures = 0;

#define LOC_CHECK(cond, msg)                                                 \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::cerr << "  FAIL: " << (msg) << " (" << __FILE__ << ":" << __LINE__ \
                << ")" << std::endl;                                         \
      ++g_failures;                                                          \
    } else {                                                                 \
      std::cout << "  OK: " << (msg) << std::endl;                           \
    }                                                                        \
  } while (0)

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
  bool WaitFor(uint32_t msg_id, int timeout_ms) {
    std::unique_lock<std::mutex> lk(mu_);
    return cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] {
      for (auto& m : msgs_) {
        if (m.msg_id == msg_id) return true;
      }
      return false;
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
    thread_ = std::thread([this, port] {
      // GameServer 依赖 enable_shared_from_this（RegisterAllHandlers 的 weak_from_this）
      server_ = std::make_shared<GameServer>("127.0.0.1", port);
      server_->Start();
      server_->Loop();
      server_.reset();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    return true;
  }
  void Stop() {
    if (server_) server_->Stop();
    if (thread_.joinable()) thread_.join();
  }

 private:
  std::shared_ptr<GameServer> server_;
  std::thread thread_;
};

class LocoClient {
 public:
  LocoClient(const std::string& ip, int port, const std::string& uid)
      : ip_(ip),
        port_(port),
        uid_(uid),
        msg_queue_(std::make_shared<MsgQueue>()) {}
  ~LocoClient() { Stop(); }

  void Start() {
    thread_ = std::thread([this] {
      EventLoop loop;
      TcpClient client(&loop, ip_, port_, nullptr);
      loop_ = &loop;
      client.SetConnectionCallback(
          [this](const std::shared_ptr<TcpConnection>& c) { OnConnection(c); });
      client.SetMessageCallback(
          [this](const std::shared_ptr<TcpConnection>&, Buffer* b) {
            OnMessage(b);
          });
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
        if (conn_ && conn_->Connected()) conn_->Shutdown();
      });
      std::this_thread::sleep_for(std::chrono::milliseconds(80));
      loop_->Quit();
    }
    if (thread_.joinable()) thread_.join();
    loop_ = nullptr;
  }

  bool WaitInGame(int ms = 10000) {
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start)
               .count() < ms) {
      if (in_game_) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return in_game_;
  }

  std::shared_ptr<MsgQueue> msg_queue() { return msg_queue_; }

  void SendMove(float x, float y, float z, ::move_status st = ::MOVE_STATUS_WALK) {
    if (!loop_) return;
    loop_->RunInLoop([this, x, y, z, st] {
      if (!conn_ || !conn_->Connected()) return;
      ::cli_3d_move_req req;
      auto* m = req.mutable_move();
      m->mutable_pos()->set_x(x);
      m->mutable_pos()->set_y(y);
      m->mutable_pos()->set_z(z);
      m->mutable_rot()->set_w(1);
      m->mutable_move_rot()->set_w(1);
      m->set_status(st);
      SendFrame(conn_, proto_id("cli_3d_move_req"), req, &seq_);
    });
  }

  void SendJump(uint32_t jump_id, ::jump_op op, ::jump_type type, float x,
                float y, float z, float vy = 0.f, uint32_t air_idx = 0) {
    if (!loop_) return;
    loop_->RunInLoop([this, jump_id, op, type, x, y, z, vy, air_idx] {
      if (!conn_ || !conn_->Connected()) return;
      ::cli_3d_jump_req req;
      auto* j = req.mutable_jump();
      j->set_jump_id(jump_id);
      j->set_op(op);
      j->set_type(type);
      j->set_air_jump_index(air_idx);
      j->mutable_pos()->set_x(x);
      j->mutable_pos()->set_y(y);
      j->mutable_pos()->set_z(z);
      j->mutable_rot()->set_w(1);
      j->mutable_velocity()->set_y(vy);
      j->set_client_time(1);
      SendFrame(conn_, proto_id("cli_3d_jump_req"), req, &seq_);
    });
  }

  // 返回最近一帧 jump_res 的 err_code；找不到返回 -1
  int WaitJumpErr(int timeout_ms = 5000) {
    if (!msg_queue_->WaitFor(proto_id("cli_3d_jump_res"), timeout_ms)) {
      return -1;
    }
    int err = -1;
    for (auto& m : msg_queue_->Drain()) {
      if (m.msg_id != proto_id("cli_3d_jump_res")) continue;
      ::cli_3d_jump_res rsp;
      if (rsp.ParseFromString(m.body)) err = rsp.err_code();
    }
    return err;
  }

  int WaitMoveErr(int timeout_ms = 5000) {
    if (!msg_queue_->WaitFor(proto_id("cli_3d_move_res"), timeout_ms)) {
      return -1;
    }
    int err = -1;
    for (auto& m : msg_queue_->Drain()) {
      if (m.msg_id != proto_id("cli_3d_move_res")) continue;
      ::cli_3d_move_res rsp;
      if (rsp.ParseFromString(m.body)) err = rsp.err_code();
    }
    return err;
  }

 private:
  void OnConnection(const std::shared_ptr<TcpConnection>& conn) {
    if (!conn->Connected()) return;
    conn_ = conn;
    socket::SetKeepAlive(conn->GetSockfd(), 1);
    ::cli_handshake_req req;
    req.set_version("locomotion/1.0");
    SendFrame(conn_, proto_id("cli_handshake_req"), req, &seq_);
  }

  void OnMessage(Buffer* buf) {
    std::vector<PackFrame> frames;
    if (!TryDecodeFrames(buf, frames)) return;
    for (const auto& f : frames) {
      msg_queue_->Push(f.msg_id, f.body);
      HandleFrame(f);
    }
  }

  void HandleFrame(const PackFrame& f) {
    if (f.msg_id == proto_id("cli_handshake_res")) {
      ::cli_user_login_req req;
      req.set_uid(uid_);
      req.set_token("tok");
      req.set_channel_id(1);
      SendFrame(conn_, proto_id("cli_user_login_req"), req, &seq_);
    } else if (f.msg_id == proto_id("cli_user_login_res")) {
      ::cli_role_list_req req;
      SendFrame(conn_, proto_id("cli_role_list_req"), req, &seq_);
    } else if (f.msg_id == proto_id("cli_role_list_res")) {
      ::cli_role_list_res rsp;
      if (!rsp.ParseFromString(f.body)) return;
      if (rsp.role_list_size() == 0) {
        ::cli_role_create_req creq;
        auto* info = creq.mutable_role_info();
        info->set_name("LocoHero");
        info->set_sex(1);
        info->set_job(1);
        SendFrame(conn_, proto_id("cli_role_create_req"), creq, &seq_);
        return;
      }
      ::cli_role_login_req req;
      req.set_role_id(rsp.role_list(0).role_id());
      req.set_op_code(1);
      SendFrame(conn_, proto_id("cli_role_login_req"), req, &seq_);
    } else if (f.msg_id == proto_id("cli_role_login_res")) {
      ::cli_enter_game_req req;
      SendFrame(conn_, proto_id("cli_enter_game_req"), req, &seq_);
    } else if (f.msg_id == proto_id("cli_enter_game_res")) {
      ::cli_enter_game_res rsp;
      if (rsp.ParseFromString(f.body) && rsp.err_code() == 0) {
        in_game_ = true;
      }
    }
  }

  std::string ip_;
  int port_ = 0;
  std::string uid_;
  std::shared_ptr<MsgQueue> msg_queue_;
  std::thread thread_;
  EventLoop* loop_ = nullptr;
  std::shared_ptr<TcpConnection> conn_;
  uint8_t seq_ = 0;
  std::atomic<bool> in_game_{false};
  std::atomic<bool> stopped_{false};
  std::mutex start_mu_;
  std::condition_variable start_cv_;
  bool ready_ = false;
};

constexpr float kBornX = 333.0f;
constexpr float kBornY = 18.0f;
constexpr float kBornZ = 415.45f;

void SleepMs(int ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

bool Test_NormalJumpLandThenMove() {
  g_failures = 0;
  std::cout << "\n[Loco] NormalJumpLandThenMove" << std::endl;
  const int port = 23101;
  TestServer server;
  server.Start(port);
  LocoClient c("127.0.0.1", port, "loco_normal");
  c.Start();
  LOC_CHECK(c.WaitInGame(), "enter game");
  c.msg_queue()->Drain();

  c.SendJump(1001, ::JUMP_START, ::JUMP_TYPE_NORMAL, kBornX, kBornY + 2.f,
             kBornZ, 8.f);
  LOC_CHECK(c.WaitJumpErr() == server::kJumpErrOk, "normal start ok");

  c.SendJump(1001, ::JUMP_STEER, ::JUMP_TYPE_NORMAL, kBornX + 1.f,
             kBornY + 3.f, kBornZ, 4.f);
  LOC_CHECK(c.WaitJumpErr() == server::kJumpErrOk, "steer ok");

  c.SendJump(1001, ::JUMP_LAND, ::JUMP_TYPE_NORMAL, kBornX + 1.f, kBornY,
             kBornZ);
  LOC_CHECK(c.WaitJumpErr() == server::kJumpErrOk, "land ok");

  c.SendMove(kBornX + 2.f, kBornY, kBornZ);
  LOC_CHECK(c.WaitMoveErr() == server::kMoveErrOk, "move after land ok");

  c.Stop();
  server.Stop();
  return g_failures == 0;
}

bool Test_DoubleJumpSuccess() {
  g_failures = 0;
  std::cout << "\n[Loco] DoubleJumpSuccess" << std::endl;
  const int port = 23102;
  TestServer server;
  server.Start(port);
  LocoClient c("127.0.0.1", port, "loco_double");
  c.Start();
  LOC_CHECK(c.WaitInGame(), "enter game");
  c.msg_queue()->Drain();

  c.SendJump(2001, ::JUMP_START, ::JUMP_TYPE_NORMAL, kBornX, kBornY + 2.f,
             kBornZ, 8.f);
  LOC_CHECK(c.WaitJumpErr() == server::kJumpErrOk, "first start");
  SleepMs(350);  // 冷却

  c.SendJump(2001, ::JUMP_START, ::JUMP_TYPE_DOUBLE, kBornX, kBornY + 4.f,
             kBornZ, 10.f, 1);
  LOC_CHECK(c.WaitJumpErr() == server::kJumpErrOk, "double start");

  c.SendJump(2001, ::JUMP_LAND, ::JUMP_TYPE_DOUBLE, kBornX, kBornY, kBornZ);
  LOC_CHECK(c.WaitJumpErr() == server::kJumpErrOk, "land after double");

  c.Stop();
  server.Stop();
  return g_failures == 0;
}

bool Test_DoubleJumpWithoutAirFails() {
  g_failures = 0;
  std::cout << "\n[Loco] DoubleJumpWithoutAirFails" << std::endl;
  const int port = 23103;
  TestServer server;
  server.Start(port);
  LocoClient c("127.0.0.1", port, "loco_dbl_ground");
  c.Start();
  LOC_CHECK(c.WaitInGame(), "enter game");
  c.msg_queue()->Drain();

  c.SendJump(3001, ::JUMP_START, ::JUMP_TYPE_DOUBLE, kBornX, kBornY + 2.f,
             kBornZ, 8.f, 1);
  LOC_CHECK(c.WaitJumpErr() == server::kJumpErrBadAirState,
            "double on ground rejected");

  c.Stop();
  server.Stop();
  return g_failures == 0;
}

bool Test_CliffFallSeparateFromMove() {
  g_failures = 0;
  std::cout << "\n[Loco] CliffFallSeparateFromMove" << std::endl;
  const int port = 23104;
  TestServer server;
  server.Start(port);
  LocoClient c("127.0.0.1", port, "loco_cliff");
  c.Start();
  LOC_CHECK(c.WaitInGame(), "enter game");
  c.msg_queue()->Drain();

  // 禁止用 move 报 FALL
  c.SendMove(kBornX + 5.f, kBornY - 1.f, kBornZ, ::MOVE_STATUS_FALL);
  LOC_CHECK(c.WaitMoveErr() == server::kMoveErrUseJumpProto,
            "move+FALL rejected");

  // 正确：jump FALL
  c.SendJump(4001, ::JUMP_START, ::JUMP_TYPE_FALL, kBornX + 5.f, kBornY - 1.f,
             kBornZ, -2.f);
  LOC_CHECK(c.WaitJumpErr() == server::kJumpErrOk, "fall start ok");

  // 下落中禁止 move
  c.SendMove(kBornX + 6.f, kBornY - 3.f, kBornZ, ::MOVE_STATUS_WALK);
  LOC_CHECK(c.WaitMoveErr() == server::kMoveErrAirborne,
            "move while falling rejected");

  c.SendJump(4001, ::JUMP_STEER, ::JUMP_TYPE_FALL, kBornX + 6.f, kBornY - 5.f,
             kBornZ, -8.f);
  LOC_CHECK(c.WaitJumpErr() == server::kJumpErrOk, "fall steer ok");

  c.SendJump(4001, ::JUMP_LAND, ::JUMP_TYPE_FALL, kBornX + 6.f, kBornY - 10.f,
             kBornZ);
  LOC_CHECK(c.WaitJumpErr() == server::kJumpErrOk, "fall land ok");

  c.SendMove(kBornX + 7.f, kBornY - 10.f, kBornZ);
  LOC_CHECK(c.WaitMoveErr() == server::kMoveErrOk, "move after fall land ok");

  c.Stop();
  server.Stop();
  return g_failures == 0;
}

bool Test_SlopeSlide() {
  g_failures = 0;
  std::cout << "\n[Loco] SlopeSlide" << std::endl;
  const int port = 23105;
  TestServer server;
  server.Start(port);
  LocoClient c("127.0.0.1", port, "loco_slide");
  c.Start();
  LOC_CHECK(c.WaitInGame(), "enter game");
  c.msg_queue()->Drain();

  c.SendMove(kBornX, kBornY, kBornZ, ::MOVE_STATUS_SLIDE);
  LOC_CHECK(c.WaitMoveErr() == server::kMoveErrUseJumpProto,
            "move+SLIDE rejected");

  c.SendJump(5001, ::JUMP_START, ::JUMP_TYPE_SLIDE, kBornX + 1.f, kBornY - 0.5f,
             kBornZ, -1.f);
  LOC_CHECK(c.WaitJumpErr() == server::kJumpErrOk, "slide start");

  c.SendJump(5001, ::JUMP_STEER, ::JUMP_TYPE_SLIDE, kBornX + 3.f, kBornY - 2.f,
             kBornZ, -3.f);
  LOC_CHECK(c.WaitJumpErr() == server::kJumpErrOk, "slide steer");

  c.SendMove(kBornX + 4.f, kBornY - 2.5f, kBornZ);
  LOC_CHECK(c.WaitMoveErr() == server::kMoveErrAirborne,
            "move while sliding rejected");

  c.SendJump(5001, ::JUMP_LAND, ::JUMP_TYPE_SLIDE, kBornX + 4.f, kBornY - 3.f,
             kBornZ);
  LOC_CHECK(c.WaitJumpErr() == server::kJumpErrOk, "slide land");

  c.Stop();
  server.Stop();
  return g_failures == 0;
}

bool Test_MoveWhileJumpingRejected() {
  g_failures = 0;
  std::cout << "\n[Loco] MoveWhileJumpingRejected" << std::endl;
  const int port = 23106;
  TestServer server;
  server.Start(port);
  LocoClient c("127.0.0.1", port, "loco_air_move");
  c.Start();
  LOC_CHECK(c.WaitInGame(), "enter game");
  c.msg_queue()->Drain();

  c.SendJump(6001, ::JUMP_START, ::JUMP_TYPE_NORMAL, kBornX, kBornY + 2.f,
             kBornZ, 8.f);
  LOC_CHECK(c.WaitJumpErr() == server::kJumpErrOk, "jump start");

  c.SendMove(kBornX + 1.f, kBornY + 2.f, kBornZ, ::MOVE_STATUS_RUN);
  LOC_CHECK(c.WaitMoveErr() == server::kMoveErrAirborne,
            "move mid-jump rejected");

  c.SendMove(kBornX + 1.f, kBornY + 2.f, kBornZ, ::MOVE_STATUS_JUMP);
  LOC_CHECK(c.WaitMoveErr() == server::kMoveErrUseJumpProto,
            "move+JUMP status rejected");

  c.SendJump(6001, ::JUMP_LAND, ::JUMP_TYPE_NORMAL, kBornX, kBornY, kBornZ);
  LOC_CHECK(c.WaitJumpErr() == server::kJumpErrOk, "land");

  c.Stop();
  server.Stop();
  return g_failures == 0;
}

bool Test_AirJumpExhausted() {
  g_failures = 0;
  std::cout << "\n[Loco] AirJumpExhausted" << std::endl;
  const int port = 23107;
  TestServer server;
  server.Start(port);
  LocoClient c("127.0.0.1", port, "loco_exhaust");
  c.Start();
  LOC_CHECK(c.WaitInGame(), "enter game");
  c.msg_queue()->Drain();

  c.SendJump(7001, ::JUMP_START, ::JUMP_TYPE_NORMAL, kBornX, kBornY + 2.f,
             kBornZ, 8.f);
  LOC_CHECK(c.WaitJumpErr() == server::kJumpErrOk, "start1");
  SleepMs(350);
  c.SendJump(7001, ::JUMP_START, ::JUMP_TYPE_DOUBLE, kBornX, kBornY + 4.f,
             kBornZ, 10.f, 1);
  LOC_CHECK(c.WaitJumpErr() == server::kJumpErrOk, "double ok");
  SleepMs(350);
  c.SendJump(7001, ::JUMP_START, ::JUMP_TYPE_DOUBLE, kBornX, kBornY + 5.f,
             kBornZ, 10.f, 2);
  LOC_CHECK(c.WaitJumpErr() == server::kJumpErrAirJumpExhausted,
            "third air jump rejected");

  c.SendJump(7001, ::JUMP_LAND, ::JUMP_TYPE_DOUBLE, kBornX, kBornY, kBornZ);
  LOC_CHECK(c.WaitJumpErr() == server::kJumpErrOk, "land");

  c.Stop();
  server.Stop();
  return g_failures == 0;
}

bool Test_FallThenDoubleJump() {
  g_failures = 0;
  std::cout << "\n[Loco] FallThenDoubleJump" << std::endl;
  const int port = 23108;
  TestServer server;
  server.Start(port);
  LocoClient c("127.0.0.1", port, "loco_fall_dbl");
  c.Start();
  LOC_CHECK(c.WaitInGame(), "enter game");
  c.msg_queue()->Drain();

  c.SendJump(8001, ::JUMP_START, ::JUMP_TYPE_FALL, kBornX + 2.f, kBornY - 1.f,
             kBornZ, -1.f);
  LOC_CHECK(c.WaitJumpErr() == server::kJumpErrOk, "fall start");
  SleepMs(350);
  c.SendJump(8001, ::JUMP_START, ::JUMP_TYPE_DOUBLE, kBornX + 2.f, kBornY + 1.f,
             kBornZ, 9.f, 1);
  LOC_CHECK(c.WaitJumpErr() == server::kJumpErrOk, "double from fall");
  c.SendJump(8001, ::JUMP_LAND, ::JUMP_TYPE_DOUBLE, kBornX + 2.f, kBornY,
             kBornZ);
  LOC_CHECK(c.WaitJumpErr() == server::kJumpErrOk, "land");

  c.Stop();
  server.Stop();
  return g_failures == 0;
}

bool Test_JumpIdMismatch() {
  g_failures = 0;
  std::cout << "\n[Loco] JumpIdMismatch" << std::endl;
  const int port = 23109;
  TestServer server;
  server.Start(port);
  LocoClient c("127.0.0.1", port, "loco_jid");
  c.Start();
  LOC_CHECK(c.WaitInGame(), "enter game");
  c.msg_queue()->Drain();

  c.SendJump(9001, ::JUMP_START, ::JUMP_TYPE_NORMAL, kBornX, kBornY + 2.f,
             kBornZ, 8.f);
  LOC_CHECK(c.WaitJumpErr() == server::kJumpErrOk, "start");
  c.SendJump(9999, ::JUMP_STEER, ::JUMP_TYPE_NORMAL, kBornX, kBornY + 3.f,
             kBornZ, 4.f);
  LOC_CHECK(c.WaitJumpErr() == server::kJumpErrJumpIdMismatch,
            "steer wrong jump_id");
  c.SendJump(9001, ::JUMP_LAND, ::JUMP_TYPE_NORMAL, kBornX, kBornY, kBornZ);
  LOC_CHECK(c.WaitJumpErr() == server::kJumpErrOk, "land");

  c.Stop();
  server.Stop();
  return g_failures == 0;
}

}  // namespace

int main() {
  InitSignals();
  setenv("GAME_HEARTBEAT_TIMEOUT_SEC", "60", 1);
  setenv("GAME_HEARTBEAT_CHECK_INTERVAL_SEC", "5", 1);
  zrpc::Logger::SetLogLevel(zrpc::Logger::WARN);

  std::cout << "=== svc_game_3d_protocol_locomotion_test ===" << std::endl;

  struct Case {
    const char* name;
    bool (*fn)();
  };
  Case cases[] = {
      {"NormalJumpLandThenMove", Test_NormalJumpLandThenMove},
      {"DoubleJumpSuccess", Test_DoubleJumpSuccess},
      {"DoubleJumpWithoutAirFails", Test_DoubleJumpWithoutAirFails},
      {"CliffFallSeparateFromMove", Test_CliffFallSeparateFromMove},
      {"SlopeSlide", Test_SlopeSlide},
      {"MoveWhileJumpingRejected", Test_MoveWhileJumpingRejected},
      {"AirJumpExhausted", Test_AirJumpExhausted},
      {"FallThenDoubleJump", Test_FallThenDoubleJump},
      {"JumpIdMismatch", Test_JumpIdMismatch},
  };

  int failed_cases = 0;
  for (auto& c : cases) {
    if (!c.fn()) {
      std::cerr << "[CASE FAIL] " << c.name << std::endl;
      ++failed_cases;
    } else {
      std::cout << "[CASE PASS] " << c.name << std::endl;
    }
  }

  std::cout << "\n=== done: " << (sizeof(cases) / sizeof(cases[0]))
            << " cases, " << failed_cases << " failed ===" << std::endl;
  return failed_cases == 0 ? 0 : 1;
}
