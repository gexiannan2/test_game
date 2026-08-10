// protocol_regression_test.cc — 协议级回归测试（验证 server bug 修复）
// 编译目标：svc_game_3d_protocol_regression_test

#include <google/protobuf/message.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "client_3d.pb.h"
#include "client_login.pb.h"
#include "protocol/pack_codec.h"
#include "protocol/pack_flags.h"
#include "common/init.h"
#include "ecs/components/connection_component.h"
#include "session/system.h"
#include "zrpc/base/buffer.h"
#include "zrpc/base/logger.h"
#include "zrpc/net/event_loop.h"
#include "zrpc/net/socket.h"
#include "zrpc/net/tcp_client.h"
#include "zrpc/net/tcp_connection.h"

#include "game_server.h"

#ifdef _WIN32
#include <winsock2.h>
#endif

using namespace zrpc;

namespace {

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
      // GameServer 注册回调时使用 weak_from_this，测试服必须由 shared_ptr 持有。
      server_ = std::make_shared<GameServer>("127.0.0.1", port);
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
  std::shared_ptr<GameServer> server_;
  std::thread thread_;
};

// 可控协议客户端：登录后进图，支持手动发 role_login / enter_game
class ManualClient {
 public:
  ManualClient(const std::string& ip, int port, const std::string& uid)
      : ip_(ip), port_(port), uid_(uid), msg_queue_(std::make_shared<MsgQueue>()) {}

  ~ManualClient() { Stop(); }

  void Start() {
    thread_ = std::thread([this] {
      EventLoop loop;
      TcpClient client(&loop, ip_, port_, nullptr);
      loop_ = &loop;
      client_ = &client;
      client.SetConnectionCallback(
          [this](const std::shared_ptr<TcpConnection>& c) { OnConnection(c); });
      client.SetMessageCallback(
          [this](const std::shared_ptr<TcpConnection>& c, Buffer* b) {
            OnMessage(c, b);
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
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      loop_->Quit();
    }
    if (thread_.joinable()) thread_.join();
    loop_ = nullptr;
    client_ = nullptr;
  }

  std::shared_ptr<MsgQueue> msg_queue() { return msg_queue_; }

  bool WaitLoggedIn(int ms = 8000) {
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start)
               .count() < ms) {
      if (logged_in_) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return logged_in_;
  }

  bool WaitInGame(int ms = 8000) {
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start)
               .count() < ms) {
      if (in_game_) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return in_game_;
  }

  void SendRoleLogin(uint64_t role_id) {
    if (!loop_) return;
    loop_->RunInLoop([this, role_id] {
      if (!conn_ || !conn_->Connected()) return;
      ::cli_role_login_req req;
      req.set_role_id(role_id);
      req.set_op_code(1);
      SendFrame(conn_, proto_id("cli_role_login_req"), req, &seq_);
    });
  }

  void SendEnterGame() {
    if (!loop_) return;
    loop_->RunInLoop([this] {
      if (!conn_ || !conn_->Connected()) return;
      ::cli_enter_game_req req;
      SendFrame(conn_, proto_id("cli_enter_game_req"), req, &seq_);
    });
  }

  void SendRoleCreate(const std::string& name, uint32_t sex, uint32_t job) {
    if (!loop_) return;
    loop_->RunInLoop([this, name, sex, job] {
      if (!conn_ || !conn_->Connected()) return;
      ::cli_role_create_req req;
      auto* info = req.mutable_role_info();
      info->set_name(name);
      info->set_sex(sex);
      info->set_job(job);
      SendFrame(conn_, proto_id("cli_role_create_req"), req, &seq_);
    });
  }

  void SendRoleDelete(uint64_t role_id) {
    if (!loop_) return;
    loop_->RunInLoop([this, role_id] {
      if (!conn_ || !conn_->Connected()) return;
      ::cli_role_delete_req req;
      req.set_role_id(role_id);
      SendFrame(conn_, proto_id("cli_role_delete_req"), req, &seq_);
    });
  }

  void SendRandomName(uint32_t sex) {
    if (!loop_) return;
    loop_->RunInLoop([this, sex] {
      if (!conn_ || !conn_->Connected()) return;
      ::cli_random_name_req req;
      req.set_sex(sex);
      SendFrame(conn_, proto_id("cli_random_name_req"), req, &seq_);
    });
  }

  void SendHandshakeOnly() {
    if (!loop_) return;
    loop_->RunInLoop([this] {
      if (!conn_ || !conn_->Connected()) return;
      ::cli_handshake_req req;
      req.set_version("regression/handshake_only");
      SendFrame(conn_, proto_id("cli_handshake_req"), req, &seq_);
    });
  }

  bool auto_enter_game_ = true;
  bool auto_login_ = true;
  bool auto_role_flow_ = true;
  bool auto_handshake_ = true;
  std::atomic<bool> got_kickoff_{false};

  uint64_t session_id() const { return session_id_.load(); }
  int last_reconnect_err() const { return last_reconnect_err_.load(); }
  bool Connected() const { return conn_ && conn_->Connected(); }

  void Login() {
    if (!loop_) return;
    loop_->RunInLoop([this] {
      if (!conn_ || !conn_->Connected()) return;
      ::cli_user_login_req req;
      req.set_uid(uid_);
      req.set_token("token");
      req.set_channel_id(1);
      SendFrame(conn_, proto_id("cli_user_login_req"), req, &seq_);
    });
  }

  void SendMove(float x, float y, float z) {
    if (!loop_) return;
    loop_->RunInLoop([this, x, y, z] {
      if (!conn_ || !conn_->Connected()) return;
      ::cli_3d_move_req req;
      auto* m = req.mutable_move();
      m->mutable_pos()->set_x(x);
      m->mutable_pos()->set_y(y);
      m->mutable_pos()->set_z(z);
      m->mutable_rot()->set_w(1);
      m->mutable_move_rot()->set_w(1);
      SendFrame(conn_, proto_id("cli_3d_move_req"), req, &seq_);
    });
  }

  void SendHeartbeat() {
    if (!loop_) return;
    loop_->RunInLoop([this] {
      if (!conn_ || !conn_->Connected()) return;
      ::cli_heart_beat_req req;
      SendFrame(conn_, proto_id("cli_heart_beat_req"), req, &seq_);
    });
  }

  void SendReconnect(uint64_t sid) {
    if (!loop_) return;
    loop_->RunInLoop([this, sid] {
      if (!conn_ || !conn_->Connected()) return;
      ::cli_reconnect_req req;
      req.set_session_id(sid);
      SendFrame(conn_, proto_id("cli_reconnect_req"), req, &seq_);
    });
  }

  void SendRawBytes(const std::string& raw) {
    if (!loop_) return;
    loop_->RunInLoop([this, raw] {
      if (!conn_ || !conn_->Connected()) return;
      Buffer out;
      out.Append(raw.data(), raw.size());
      conn_->Send(&out);
    });
  }

  void SendLoginWithoutHandshake() {
    if (!loop_) return;
    loop_->RunInLoop([this] {
      if (!conn_ || !conn_->Connected()) return;
      ::cli_user_login_req req;
      req.set_uid(uid_);
      req.set_token("token");
      req.set_channel_id(1);
      SendFrame(conn_, proto_id("cli_user_login_req"), req, &seq_);
    });
  }

 private:
  void OnConnection(const std::shared_ptr<TcpConnection>& conn) {
    if (conn->Connected()) {
      conn_ = conn;
      socket::SetKeepAlive(conn->GetSockfd(), 1);
      if (auto_handshake_) {
        ::cli_handshake_req req;
        req.set_version("regression/1.0");
        SendFrame(conn_, proto_id("cli_handshake_req"), req, &seq_);
      }
    }
  }

  void OnMessage(const std::shared_ptr<TcpConnection>&, Buffer* buf) {
    std::vector<PackFrame> frames;
    if (!TryDecodeFrames(buf, frames)) return;
    for (const auto& f : frames) {
      msg_queue_->Push(f.msg_id, f.body);
      HandleFrame(f);
    }
  }

  void HandleFrame(const PackFrame& f) {
    if (f.msg_id == proto_id("cli_handshake_res")) {
      ::cli_handshake_res rsp;
      if (rsp.ParseFromString(f.body) && rsp.err_code() == 0 && auto_login_) {
        ::cli_user_login_req req;
        req.set_uid(uid_);
        req.set_token("token");
        req.set_channel_id(1);
        SendFrame(conn_, proto_id("cli_user_login_req"), req, &seq_);
      }
    } else if (f.msg_id == proto_id("cli_user_login_res")) {
      ::cli_user_login_res rsp;
      if (rsp.ParseFromString(f.body) && rsp.err_code() == 0) {
        logged_in_ = true;
        session_id_ = rsp.session_id();
        ::cli_role_list_req req;
        SendFrame(conn_, proto_id("cli_role_list_req"), req, &seq_);
      }
    } else if (f.msg_id == proto_id("cli_role_list_res")) {
      if (auto_role_flow_) {
        ::cli_role_list_res rsp;
        if (rsp.ParseFromString(f.body)) {
          if (rsp.role_list_size() > 0) {
            SendRoleLogin(rsp.role_list(0).role_id());
          } else {
            ::cli_role_create_req req;
            auto* info = req.mutable_role_info();
            info->set_name("Reg_" + uid_);
            info->set_sex(1);
            info->set_job(1);
            SendFrame(conn_, proto_id("cli_role_create_req"), req, &seq_);
          }
        }
      }
    } else if (f.msg_id == proto_id("cli_role_login_res")) {
      ::cli_role_login_res rsp;
      if (rsp.ParseFromString(f.body) && rsp.err_code() == 0 && auto_enter_game_) {
        ::cli_enter_game_req req;
        SendFrame(conn_, proto_id("cli_enter_game_req"), req, &seq_);
      }
    } else if (f.msg_id == proto_id("cli_3d_enter_map_ntf")) {
      ::cli_3d_enter_map_ntf rsp;
      if (rsp.ParseFromString(f.body) && rsp.err_code() == 0) in_game_ = true;
    } else if (f.msg_id == proto_id("cli_kickoff_player_ntf")) {
      got_kickoff_ = true;
    } else if (f.msg_id == proto_id("cli_reconnect_res")) {
      ::cli_reconnect_res rsp;
      if (rsp.ParseFromString(f.body)) {
        last_reconnect_err_ = rsp.err_code();
      }
    }
  }

  std::string ip_, uid_;
  int port_;
  EventLoop* loop_ = nullptr;
  TcpClient* client_ = nullptr;
  std::mutex start_mu_;
  std::condition_variable start_cv_;
  bool ready_ = false;
  std::thread thread_;
  std::shared_ptr<TcpConnection> conn_;
  uint8_t seq_ = 0;
  std::atomic<bool> logged_in_{false};
  std::atomic<bool> in_game_{false};
  std::atomic<bool> stopped_{false};
  std::atomic<uint64_t> session_id_{0};
  std::atomic<int> last_reconnect_err_{-1};
  std::shared_ptr<MsgQueue> msg_queue_;
};

int g_failures = 0;

#define REG_CHECK(cond, msg) \
  do { \
    if (!(cond)) { \
      std::cerr << "[FAIL] " << msg << " @ " << __FILE__ << ":" << __LINE__ << std::endl; \
      ++g_failures; \
    } \
  } while (0)

bool Test_InvalidRoleLoginRejected() {
  g_failures = 0;
  std::cout << "\n[RegTest] InvalidRoleLoginRejected" << std::endl;
  const int port = 22001;
  TestServer server;
  server.Start(port);

  ManualClient c("127.0.0.1", port, "reg_invalid_role");
  c.auto_role_flow_ = false;
  c.auto_enter_game_ = false;
  c.Start();
  REG_CHECK(c.WaitLoggedIn(), "login failed");

  c.msg_queue()->Drain();
  c.SendRoleLogin(88888888);
  bool got = c.msg_queue()->WaitFor(proto_id("cli_role_login_res"), 5000);
  REG_CHECK(got, "no role_login_res");

  if (got) {
    ::cli_role_login_res rsp;
    bool parsed = false;
    for (auto& m : c.msg_queue()->Drain()) {
      if (m.msg_id == proto_id("cli_role_login_res")) {
        parsed = rsp.ParseFromString(m.body);
        break;
      }
    }
    REG_CHECK(parsed, "parse role_login_res failed");
    if (parsed) REG_CHECK(rsp.err_code() != 0, "invalid role_id should be rejected");
  }

  c.Stop();
  server.Stop();
  return g_failures == 0;
}

bool Test_DeleteRoleKeepsRelogin() {
  g_failures = 0;
  std::cout << "\n[RegTest] DeleteRoleKeepsRelogin" << std::endl;
  const int port = 22002;
  TestServer server;
  server.Start(port);

  ManualClient c("127.0.0.1", port, "reg_delete_relogin");
  c.auto_enter_game_ = false;
  c.auto_role_flow_ = false;
  c.Start();
  REG_CHECK(c.WaitLoggedIn(), "login failed");

  c.msg_queue()->Drain();
  c.SendRoleCreate("HeroA", 1, 1);
  REG_CHECK(c.msg_queue()->WaitFor(proto_id("cli_role_list_res"), 5000), "create A failed");

  uint64_t role_a = 0;
  uint64_t role_b = 0;
  for (auto& m : c.msg_queue()->Drain()) {
    if (m.msg_id == proto_id("cli_role_list_res")) {
      ::cli_role_list_res rsp;
      if (rsp.ParseFromString(m.body) && rsp.role_list_size() > 0) {
        role_a = rsp.role_list(0).role_id();
      }
    }
  }
  REG_CHECK(role_a != 0, "role_a id missing");

  c.SendRoleCreate("HeroB", 2, 2);
  REG_CHECK(c.msg_queue()->WaitFor(proto_id("cli_role_list_res"), 5000), "create B failed");
  for (auto& m : c.msg_queue()->Drain()) {
    if (m.msg_id == proto_id("cli_role_list_res")) {
      ::cli_role_list_res rsp;
      if (rsp.ParseFromString(m.body)) {
        for (int i = 0; i < rsp.role_list_size(); ++i) {
          if (rsp.role_list(i).role_id() != role_a) {
            role_b = rsp.role_list(i).role_id();
          }
        }
      }
    }
  }
  REG_CHECK(role_b != 0, "role_b id missing");

  c.SendRoleDelete(role_a);
  REG_CHECK(c.msg_queue()->WaitFor(proto_id("cli_role_list_res"), 5000), "delete failed");

  c.SendRoleLogin(role_b);
  REG_CHECK(c.msg_queue()->WaitFor(proto_id("cli_role_login_res"), 5000), "login B failed");
  ::cli_role_login_res login_rsp;
  for (auto& m : c.msg_queue()->Drain()) {
    if (m.msg_id == proto_id("cli_role_login_res") && login_rsp.ParseFromString(m.body)) break;
  }
  REG_CHECK(login_rsp.err_code() == 0, "login B err_code != 0");
  REG_CHECK(login_rsp.role_id() == role_b, "login B wrong role_id");

  c.Stop();
  server.Stop();
  return g_failures == 0;
}

bool Test_DoubleEnterGameRejected() {
  g_failures = 0;
  std::cout << "\n[RegTest] DoubleEnterGameRejected" << std::endl;
  const int port = 22003;
  TestServer server;
  server.Start(port);

  ManualClient c("127.0.0.1", port, "reg_double_enter");
  c.Start();
  REG_CHECK(c.WaitInGame(), "first enter failed");

  c.msg_queue()->Drain();
  c.SendEnterGame();
  bool got = c.msg_queue()->WaitFor(proto_id("cli_enter_game_res"), 5000);
  REG_CHECK(got, "no second enter_game_res");

  if (got) {
    ::cli_enter_game_res rsp;
    bool found_reject = false;
    for (auto& m : c.msg_queue()->Drain()) {
      if (m.msg_id == proto_id("cli_enter_game_res") && rsp.ParseFromString(m.body)) {
        if (rsp.err_code() == 3) found_reject = true;
      }
    }
    REG_CHECK(found_reject, "double enter should return err_code=3");
  }

  c.Stop();
  server.Stop();
  return g_failures == 0;
}

bool Test_NegativeMoveAccepted() {
  g_failures = 0;
  std::cout << "\n[RegTest] NegativeMoveAccepted" << std::endl;
  const int port = 22004;
  TestServer server;
  server.Start(port);

  ManualClient c("127.0.0.1", port, "reg_neg_move");
  c.Start();
  REG_CHECK(c.WaitInGame(), "enter game failed");

  c.msg_queue()->Drain();
  c.SendMove(-100.f, 10.f, -50.f);
  bool got_move_res = c.msg_queue()->WaitFor(proto_id("cli_3d_move_res"), 5000);
  REG_CHECK(got_move_res, "negative coords should get move_res");

  c.Stop();
  server.Stop();
  return g_failures == 0;
}

bool Test_RandomNameRequiresLogin() {
  g_failures = 0;
  std::cout << "\n[RegTest] RandomNameRequiresLogin" << std::endl;
  const int port = 22005;
  TestServer server;
  server.Start(port);

  ManualClient c("127.0.0.1", port, "reg_random_name");
  c.auto_login_ = false;
  c.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(800));
  c.msg_queue()->Drain();
  c.SendRandomName(1);
  bool got_before = c.msg_queue()->WaitFor(proto_id("cli_random_name_res"), 2000);
  REG_CHECK(got_before, "random_name_res err before login");
  if (got_before) {
    ::cli_random_name_res res;
    bool found_reject = false;
    for (auto& m : c.msg_queue()->Drain()) {
      if (m.msg_id == proto_id("cli_random_name_res") && res.ParseFromString(m.body)) {
        if (res.err_code() != 0) found_reject = true;
      }
    }
    REG_CHECK(found_reject, "err_code should be non-zero before login");
  }

  c.auto_login_ = true;
  c.auto_role_flow_ = false;
  c.Login();
  REG_CHECK(c.WaitLoggedIn(), "login after handshake");

  c.msg_queue()->Drain();
  c.SendRandomName(1);
  bool got_after = c.msg_queue()->WaitFor(proto_id("cli_random_name_res"), 5000);
  REG_CHECK(got_after, "random_name_res after login");
  if (got_after) {
    ::cli_random_name_res res;
    bool found_ok = false;
    for (auto& m : c.msg_queue()->Drain()) {
      if (m.msg_id == proto_id("cli_random_name_res") && res.ParseFromString(m.body)) {
        if (res.err_code() == 0) found_ok = true;
      }
    }
    REG_CHECK(found_ok, "err_code should be 0 after login");
  }

  c.Stop();
  server.Stop();
  return g_failures == 0;
}

bool Test_KickoffOldConnDoesNotDropNew() {
  g_failures = 0;
  std::cout << "\n[RegTest] KickoffOldConnDoesNotDropNew" << std::endl;
  const int port = 22006;
  TestServer server;
  server.Start(port);

  ManualClient a("127.0.0.1", port, "reg_kick_uid");
  a.Start();
  REG_CHECK(a.WaitInGame(), "A enter failed");

  ManualClient b("127.0.0.1", port, "reg_kick_uid");
  b.Start();
  REG_CHECK(b.WaitInGame(), "B enter failed");
  REG_CHECK(a.got_kickoff_.load() || !a.Connected(), "A should be kicked");

  // 旧连接晚到的 Shutdown 不应把 B 打出图
  a.Stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  REG_CHECK(b.WaitInGame(1000), "B should remain in game after A teardown");
  b.SendMove(340.f, 18.f, 420.f);
  REG_CHECK(b.msg_queue()->WaitFor(proto_id("cli_3d_move_res"), 5000), "B move after kick");

  b.Stop();
  server.Stop();
  return g_failures == 0;
}

bool Test_ReconnectSessionValidAndInvalid() {
  g_failures = 0;
  std::cout << "\n[RegTest] ReconnectSessionValidAndInvalid" << std::endl;
  const int port = 22007;
  TestServer server;
  server.Start(port);

  ManualClient c("127.0.0.1", port, "reg_reconnect");
  c.Start();
  REG_CHECK(c.WaitLoggedIn(), "login failed");
  const uint64_t sid = c.session_id();
  REG_CHECK(sid != 0, "session_id missing");

  ManualClient bad("127.0.0.1", port, "reg_reconnect_bad");
  bad.auto_login_ = false;
  bad.auto_role_flow_ = false;
  bad.auto_enter_game_ = false;
  bad.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  bad.msg_queue()->Drain();
  bad.SendReconnect(99999999);
  REG_CHECK(bad.msg_queue()->WaitFor(proto_id("cli_reconnect_res"), 5000), "no reconnect_res");
  REG_CHECK(bad.last_reconnect_err() != 0, "invalid session should fail");

  // 同 uid 新连接用合法 session 重连
  ManualClient good("127.0.0.1", port, "reg_reconnect");
  good.auto_login_ = false;
  good.auto_role_flow_ = false;
  good.auto_enter_game_ = false;
  good.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  good.msg_queue()->Drain();
  good.SendReconnect(sid);
  REG_CHECK(good.msg_queue()->WaitFor(proto_id("cli_reconnect_res"), 5000),
            "no valid reconnect_res");
  REG_CHECK(good.last_reconnect_err() == 0, "valid session reconnect should ok");

  c.Stop();
  bad.Stop();
  good.Stop();
  server.Stop();
  return g_failures == 0;
}

bool Test_LoginWithoutHandshakeNoCrash() {
  g_failures = 0;
  std::cout << "\n[RegTest] LoginWithoutHandshakeNoCrash" << std::endl;
  const int port = 22008;
  TestServer server;
  server.Start(port);

  ManualClient c("127.0.0.1", port, "reg_no_hs");
  c.auto_handshake_ = false;
  c.auto_login_ = false;
  c.auto_role_flow_ = false;
  c.auto_enter_game_ = false;
  c.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  c.SendLoginWithoutHandshake();
  std::this_thread::sleep_for(std::chrono::milliseconds(800));
  // 允许拒登或静默丢弃，只要服不崩、客户端未必 logged_in
  REG_CHECK(!c.WaitLoggedIn(500), "should not fully login without handshake");

  c.Stop();
  server.Stop();
  return g_failures == 0;
}

bool Test_CorruptFrameClosesOrIgnores() {
  g_failures = 0;
  std::cout << "\n[RegTest] CorruptFrameClosesOrIgnores" << std::endl;
  const int port = 22009;
  TestServer server;
  server.Start(port);

  ManualClient c("127.0.0.1", port, "reg_corrupt");
  c.Start();
  REG_CHECK(c.WaitInGame(), "enter failed");
  c.SendRawBytes(std::string(64, '\xff'));
  std::this_thread::sleep_for(std::chrono::milliseconds(800));
  // 解码失败应关连接或忽略；服进程由后续 Stop 验证仍存活
  c.Stop();
  server.Stop();
  return g_failures == 0;
}

bool Test_NanMoveRejectedNoMoveRes() {
  g_failures = 0;
  std::cout << "\n[RegTest] NanMoveRejectedGetsFailedMoveRes" << std::endl;
  const int port = 22010;
  TestServer server;
  server.Start(port);

  ManualClient c("127.0.0.1", port, "reg_nan_move");
  c.Start();
  REG_CHECK(c.WaitInGame(), "enter failed");
  c.msg_queue()->Drain();
  c.SendMove(std::numeric_limits<float>::quiet_NaN(), 18.f, 415.f);
  bool got = c.msg_queue()->WaitFor(proto_id("cli_3d_move_res"), 3000);
  REG_CHECK(got, "NaN move should get move_res success=0");
  if (got) {
    ::cli_3d_move_res rsp;
    bool found_fail = false;
    for (auto& m : c.msg_queue()->Drain()) {
      if (m.msg_id == proto_id("cli_3d_move_res") && rsp.ParseFromString(m.body)) {
        if (rsp.err_code() != 0) found_fail = true;
      }
    }
    REG_CHECK(found_fail, "NaN move_res success must be 0");
  }

  c.Stop();
  server.Stop();
  return g_failures == 0;
}

bool Test_HeartbeatTimeoutKicks() {
  g_failures = 0;
  std::cout << "\n[RegTest] HeartbeatTimeoutKicks" << std::endl;
#ifdef _WIN32
  _putenv_s("GAME_HEARTBEAT_TIMEOUT_SEC", "2");
  _putenv_s("GAME_HEARTBEAT_CHECK_INTERVAL_SEC", "1");
#else
  setenv("GAME_HEARTBEAT_TIMEOUT_SEC", "2", 1);
  setenv("GAME_HEARTBEAT_CHECK_INTERVAL_SEC", "1", 1);
#endif
  const int port = 22011;
  TestServer server;
  server.Start(port);

  ManualClient keeper("127.0.0.1", port, "reg_hb_keep");
  keeper.Start();
  REG_CHECK(keeper.WaitInGame(), "keeper enter");

  ManualClient victim("127.0.0.1", port, "reg_hb_victim");
  victim.Start();
  REG_CHECK(victim.WaitInGame(), "victim enter");
  // 先打一拍心跳，让 last_heartbeat>0，随后停发才会超时踢线
  victim.SendHeartbeat();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  keeper.msg_queue()->Drain();

  const auto start = std::chrono::steady_clock::now();
  bool victim_gone = false;
  while (std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::steady_clock::now() - start)
             .count() < 8) {
    keeper.SendHeartbeat();
    if (!victim.Connected() ||
        keeper.msg_queue()->WaitFor(proto_id("cli_3d_aoi_disappears_ntf"), 200)) {
      victim_gone = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
  }
  REG_CHECK(victim_gone, "victim should be kicked by heartbeat timeout");

  keeper.Stop();
  victim.Stop();
  server.Stop();
#ifdef _WIN32
  _putenv_s("GAME_HEARTBEAT_TIMEOUT_SEC", "60");
  _putenv_s("GAME_HEARTBEAT_CHECK_INTERVAL_SEC", "5");
#else
  setenv("GAME_HEARTBEAT_TIMEOUT_SEC", "60", 1);
  setenv("GAME_HEARTBEAT_CHECK_INTERVAL_SEC", "5", 1);
#endif
  return g_failures == 0;
}

bool Test_LastHeartbeatZeroNotKicked() {
  g_failures = 0;
  std::cout << "\n[RegTest] LastHeartbeatZeroNotKicked" << std::endl;
#ifdef _WIN32
  _putenv_s("GAME_HEARTBEAT_TIMEOUT_SEC", "2");
  _putenv_s("GAME_HEARTBEAT_CHECK_INTERVAL_SEC", "1");
#else
  setenv("GAME_HEARTBEAT_TIMEOUT_SEC", "2", 1);
  setenv("GAME_HEARTBEAT_CHECK_INTERVAL_SEC", "1", 1);
#endif
  const int port = 22012;
  TestServer server;
  server.Start(port);

  ManualClient c("127.0.0.1", port, "reg_hb_zero");
  c.Start();
  REG_CHECK(c.WaitInGame(), "enter failed");

  // 将 last_heartbeat 清零：超时扫描应跳过
  std::atomic<bool> done{false};
  server.Get()->RunInLoop([&] {
    auto e = PlayerEntitySystem::Instance().FindByUid("reg_hb_zero");
    if (e) {
      if (auto* cc = e->GetComponent<ConnectionComponent>()) {
        cc->last_heartbeat_sec_ = 0;
      }
    }
    done = true;
  });
  for (int i = 0; i < 50 && !done.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  std::this_thread::sleep_for(std::chrono::seconds(4));
  REG_CHECK(c.Connected() && c.WaitInGame(500),
            "last_heartbeat=0 must not be kicked");

  c.Stop();
  server.Stop();
#ifdef _WIN32
  _putenv_s("GAME_HEARTBEAT_TIMEOUT_SEC", "60");
  _putenv_s("GAME_HEARTBEAT_CHECK_INTERVAL_SEC", "5");
#else
  setenv("GAME_HEARTBEAT_TIMEOUT_SEC", "60", 1);
  setenv("GAME_HEARTBEAT_CHECK_INTERVAL_SEC", "5", 1);
#endif
  return g_failures == 0;
}

bool Test_EnterDisconnectReenter() {
  g_failures = 0;
  std::cout << "\n[RegTest] EnterDisconnectReenter" << std::endl;
  const int port = 22013;
  TestServer server;
  server.Start(port);

  {
    ManualClient c("127.0.0.1", port, "reg_reenter");
    c.Start();
    REG_CHECK(c.WaitInGame(), "first enter");
    c.Stop();
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  ManualClient c2("127.0.0.1", port, "reg_reenter");
  c2.Start();
  REG_CHECK(c2.WaitInGame(), "second enter after disconnect");
  c2.Stop();
  server.Stop();
  return g_failures == 0;
}

bool Test_OrderlyServerStop() {
  g_failures = 0;
  std::cout << "\n[RegTest] OrderlyServerStop" << std::endl;
  const int port = 22014;
  TestServer server;
  server.Start(port);
  ManualClient a("127.0.0.1", port, "reg_stop_a");
  ManualClient b("127.0.0.1", port, "reg_stop_b");
  a.Start();
  b.Start();
  REG_CHECK(a.WaitInGame() && b.WaitInGame(), "both enter");
  a.Stop();
  b.Stop();
  server.Stop();  // 不 _Exit，验证 Stop/join 可完成
  REG_CHECK(true, "orderly stop");
  return g_failures == 0;
}

}  // namespace

int main(int argc, char** argv) {
  InitSignals();
  // 默认心跳保持宽松，短超时仅在 Heartbeat* 用例内临时设置
#ifdef _WIN32
  _putenv_s("GAME_HEARTBEAT_TIMEOUT_SEC", "60");
  _putenv_s("GAME_HEARTBEAT_CHECK_INTERVAL_SEC", "5");
  // 既有回归用例包含负坐标移动，不与 NavMesh 合法性测试耦合。
  _putenv_s("GAME_NAVMESH_ENABLE", "0");
#else
  setenv("GAME_HEARTBEAT_TIMEOUT_SEC", "60", 1);
  setenv("GAME_HEARTBEAT_CHECK_INTERVAL_SEC", "5", 1);
  // 既有回归用例包含负坐标移动，不与 NavMesh 合法性测试耦合。
  setenv("GAME_NAVMESH_ENABLE", "0", 1);
#endif
  zrpc::Logger::SetLogLevel(zrpc::Logger::WARN);
#ifdef _WIN32
  WSADATA wsa{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
#endif

  std::cout << "=== svc_game_3d_protocol_regression_test ===" << std::endl;

  struct Case {
    const char* name;
    bool (*fn)();
  };
  Case cases[] = {
      {"InvalidRoleLoginRejected", Test_InvalidRoleLoginRejected},
      {"DeleteRoleKeepsRelogin", Test_DeleteRoleKeepsRelogin},
      {"DoubleEnterGameRejected", Test_DoubleEnterGameRejected},
      {"NegativeMoveAccepted", Test_NegativeMoveAccepted},
      {"RandomNameRequiresLogin", Test_RandomNameRequiresLogin},
      {"KickoffOldConnDoesNotDropNew", Test_KickoffOldConnDoesNotDropNew},
      {"ReconnectSessionValidAndInvalid", Test_ReconnectSessionValidAndInvalid},
      {"LoginWithoutHandshakeNoCrash", Test_LoginWithoutHandshakeNoCrash},
      {"CorruptFrameClosesOrIgnores", Test_CorruptFrameClosesOrIgnores},
      {"NanMoveRejectedGetsFailedMoveRes", Test_NanMoveRejectedNoMoveRes},
      {"HeartbeatTimeoutKicks", Test_HeartbeatTimeoutKicks},
      {"LastHeartbeatZeroNotKicked", Test_LastHeartbeatZeroNotKicked},
      {"EnterDisconnectReenter", Test_EnterDisconnectReenter},
      {"OrderlyServerStop", Test_OrderlyServerStop},
  };

  int passed = 0;
  int total = static_cast<int>(sizeof(cases) / sizeof(cases[0]));
  for (int i = 0; i < total; ++i) {
    bool ok = cases[i].fn();
    std::cout << (ok ? "[  OK  ] " : "[ FAIL ] ") << cases[i].name << std::endl;
    if (ok) ++passed;
  }

  std::cout << "\n=== Regression: " << passed << "/" << total << " passed ==="
            << std::endl;

#ifdef _WIN32
  WSACleanup();
#endif
  return passed == total ? 0 : 1;
}
