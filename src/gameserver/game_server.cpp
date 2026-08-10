#include "game_server.h"

#include <any>
#include <iomanip>
#include <sstream>
#include <vector>

#include "PlayerMongoStorage.h"
#include "ecs/entity/entity.h"
#include "navigation/nav_system.h"
#include "protocol/pack_codec.h"
#include "protocol/pack_flags.h"
#include "server_defaults.h"
#include "zrpc/base/logger.h"
#include "zrpc/net/tcp_connection.h"

GameServer::GameServer(const std::string& ip, int port)
    : server_(&loop_, ip, static_cast<int16_t>(port), nullptr),
      ip_(ip),
      port_(port),
      next_session_id_{server::kSessionIdStart},
      next_role_id_{server::kRoleIdStart} {}

GameServer::~GameServer() {
  if (heartbeat_timer_) {
    loop_.CancelAfter(heartbeat_timer_);
    heartbeat_timer_.reset();
  }
  if (world_tick_timer_) {
    loop_.CancelAfter(world_tick_timer_);
    world_tick_timer_.reset();
  }
  if (persist_timer_) {
    loop_.CancelAfter(persist_timer_);
    persist_timer_.reset();
  }
  if (mongo_ping_timer_) {
    loop_.CancelAfter(mongo_ping_timer_);
    mongo_ping_timer_.reset();
  }
}

void GameServer::Start() {
  LoadConfigs();
  if (!InitNavigation()) {
    startup_failed_ = true;
    LOG_ERROR << "GameServer::Start aborted: navigation initialization failed";
    return;
  }
  InitWorldAndAoi();
  RegisterAllHandlers();
  InitMongoIfEnabled();
  InitServerNetwork();
  if (listen_failed_) {
    LOG_ERROR << "GameServer::Start aborted: listen failed on " << ip_ << ":"
              << port_;
    return;
  }
  StartTimers();
}

void GameServer::Loop() { loop_.Run(); }

void GameServer::Stop() {
  loop_.RunInLoop([weak = weak_from_this()] {
    auto self = weak.lock();
    if (!self) {
      return;
    }
    if (!self->stopping_.exchange(true)) {
      self->DoGracefulStop();
    }
  });
}

void GameServer::RunInLoop(std::function<void()> fn) {
  loop_.RunInLoop(std::move(fn));
}

void GameServer::RegisterHandler(uint32_t msg_id,
                                 std::unique_ptr<IHandler> handler) {
  handler->SetServer(this);
  handlers_[msg_id] = handler.get();
  handler_owners_.insert(std::move(handler));
}

IHandler* GameServer::FindHandler(uint32_t msg_id) const {
  auto it = handlers_.find(msg_id);
  if (it == handlers_.end()) {
    return nullptr;
  }
  return it->second;
}

void GameServer::OnMessage(const ::zrpc::TcpConnectionPtr& conn,
                           ::zrpc::Buffer* buf) {
  std::vector<PackFrame> frames;
  if (!TryDecodeFrames(buf, frames)) {
    const int32_t len = buf->ReadableBytes();
    const char* data = buf->Peek();
    const int32_t dump_len = (len < 80) ? len : 80;
    std::ostringstream hex;
    hex << "len=" << len << "B raw(" << dump_len << "B): ";
    for (int32_t i = 0; i < dump_len; ++i) {
      hex << std::hex << std::setw(2) << std::setfill('0')
          << (static_cast<unsigned char>(data[i]) & 0xff) << " ";
    }
    LOG_WARN << "protocol error (bad frame header/magic), close connection, "
             << hex.str();
    conn->Shutdown();
    return;
  }

  for (const auto& frame : frames) {
    auto* any_ptr = std::any_cast<EntityPtr>(&conn->GetContext());
    if (!any_ptr || !(*any_ptr)) {
      LOG_WARN << "no entity mid-batch, close connection";
      conn->Shutdown();
      return;
    }
    auto entity = *any_ptr;
    auto* handler = FindHandler(frame.msg_id);
    if (handler) {
      auto req = CreateRequest(frame.msg_id);
      if (req) {
        if (!req->ParseFromString(frame.body)) {
          LOG_WARN << "<<< [RECV] " << msg_id_name(frame.msg_id)
                   << " bad protobuf parse, drop frame";
          continue;
        }
      }
      handler->Handle(conn, entity, frame.msg_id, req);
    } else {
      LOG_WARN << "<<< [RECV] " << msg_id_name(frame.msg_id)
               << " unhandled msg_id=" << frame.msg_id;
    }
  }
}

std::shared_ptr<::google::protobuf::Message> GameServer::CreateRequest(
    uint32_t msg_id) const {
  auto it = proto_factories_.find(msg_id);
  return it != proto_factories_.end() ? it->second() : nullptr;
}

void GameServer::SendFrame(const ::zrpc::TcpConnectionPtr& conn, uint32_t msg_id,
                           const std::string& body, uint8_t* seq,
                           const std::string& proto_name) {
  if (!conn || !conn->Connected() || !seq) {
    return;
  }
  PackFrame frame;
  frame.flags = kPackFlagEncrypt;
  frame.msg_id = msg_id;
  frame.recv_index = (*seq)++;
  frame.body = body;
  ::zrpc::Buffer out;
  EncodeFrame(frame, &out);
  if (msg_id != proto_id("cli_heart_beat_res")) {
    LOG_INFO << ">>> [SEND] "
             << (proto_name.empty() ? msg_id_name(msg_id) : proto_name)
             << " msg_id=" << msg_id << " body=" << body.size()
             << "B seq=" << (uint32_t)frame.recv_index;
  }
  conn->Send(&out);
}
