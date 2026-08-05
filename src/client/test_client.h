// test_client.h — 纯客户端测试基础件（MsgQueue + TestClient）
// 用法：TestClient a("127.0.0.1", 20000, "user_A"); a.Start();
//       WaitForInGame(a); 然后通过 a.msg_queue() 收发协议帧。

#pragma once

#include <google/protobuf/message.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "client_3d.pb.h"
#include "client_login.pb.h"
#include "protocol/pack_codec.h"
#include "protocol/pack_flags.h"
#include "zrpc/base/buffer.h"
#include "zrpc/net/event_loop.h"
#include "zrpc/net/tcp_client.h"
#include "zrpc/net/tcp_connection.h"

namespace aoitest {

// ---- 帧发送 ----
inline void SendFrame(const std::shared_ptr<::zrpc::TcpConnection>& conn,
                      uint32_t msg_id,
                      const ::google::protobuf::Message& msg, uint8_t* seq) {
    PackFrame frame;
    frame.flags = kPackFlagEncrypt;
    frame.msg_id = msg_id;
    frame.recv_index = (*seq)++;
    msg.SerializeToString(&frame.body);
    ::zrpc::Buffer out;
    EncodeFrame(frame, &out);
    conn->Send(&out);
}

// ---- 收信箱 ----
struct ReceivedMsg {
    uint32_t msg_id = 0;
    std::string body;
};

class MsgQueue {
 public:
    using PushCallback = std::function<void(uint32_t msg_id, const std::string& body)>;
    void SetOnPush(PushCallback cb) { on_push_ = std::move(cb); }

    void Push(uint32_t msg_id, const std::string& body) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            msgs_.push_back({msg_id, body});
        }
        cv_.notify_all();
        if (on_push_) on_push_(msg_id, body);
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
    PushCallback on_push_;
};

// ---- 测试客户端 ----
class TestClient {
 public:
    TestClient(const std::string& ip, int port, const std::string& uid)
        : ip_(ip), port_(port), uid_(uid), msg_queue_(std::make_shared<MsgQueue>()) {}

    ~TestClient() { Stop(); }

    void Start() {
        thread_ = std::thread([this] {
            ::zrpc::EventLoop loop;
            ::zrpc::TcpClient client(&loop, ip_, port_, nullptr);
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
    const std::string& Uid() const { return uid_; }

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
            pos->set_x(x); pos->set_y(y); pos->set_z(z);
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
    void OnConnection(const std::shared_ptr<::zrpc::TcpConnection>& conn) {
        if (conn->Connected()) {
            connected_ = true;
            conn_ = conn;
            ::cli_handshake_req req;
            req.set_version("aoi_test/1.0");
            SendFrame(conn_, proto_id("cli_handshake_req"), req, &seq_);
        } else {
            connected_ = false;
        }
    }

    void OnMessage(const std::shared_ptr<::zrpc::TcpConnection>& conn,
                   ::zrpc::Buffer* buf) {
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
    ::zrpc::EventLoop* loop_ = nullptr;
    ::zrpc::TcpClient* client_ = nullptr;
    std::mutex start_mu_;
    std::condition_variable start_cv_;
    bool ready_ = false;
    std::thread thread_;
    std::shared_ptr<::zrpc::TcpConnection> conn_;
    uint8_t seq_ = 0;
    uint64_t session_id_ = 0;
    std::atomic<uint64_t> role_id_{0};
    std::atomic<bool> connected_{false};
    std::atomic<bool> in_game_{false};
    std::atomic<bool> stopped_{false};
    std::shared_ptr<MsgQueue> msg_queue_;
};

// ---- 工具函数 ----
inline bool WaitForInGame(TestClient& c, int timeout_ms = 15000) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (c.IsInGame()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return c.IsInGame();
}

// 从 mailbox 取一条指定 msg_id 的帧,解析到 proto,返回 true
template <typename T>
inline bool TakeFirst(MsgQueue& mbox, uint32_t msg_id, T* out) {
    auto msgs = mbox.Drain();
    for (auto& msg : msgs) {
        if (msg.msg_id == msg_id && out->ParseFromString(msg.body)) {
            return true;
        }
    }
    return false;
}

}  // namespace aoitest
