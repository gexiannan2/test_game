#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>

#include "archive/player_archive_service.h"
#include "ecs/entity/entity.h"
#include "ecs/systems/aoi_view_bridge.h"
#include "ecs/systems/map_view_bridge.h"
#include "ecs/systems/player_persist_system.h"
#include "ecs/systems/world_system.h"
#include "handlers/handler_base.h"
#include "jolt_server.h"
#include "protocol/pack_codec.h"
#include "server_defaults.h"
#include "session/system.h"
#include "zrpc/base/logger.h"
#include "zrpc/net/event_loop.h"
#include "zrpc/net/tcp_server.h"

namespace mongo {
class PlayerMongoStorage;
}  // namespace mongo

namespace game::navigation {
class HeightMapSystem;
class NavSystem;
}  // namespace game::navigation

// 游戏服编排：TCP 收发包、Handler 分发、WorldSystem/AoiViewBridge 生命周期
// 继承 enable_shared_from_this：所有跨线程 lambda 通过 weak_ptr 捕获
class GameServer : public std::enable_shared_from_this<GameServer> {
 public:
  static constexpr int kHeartbeatCheckIntervalSec = 5;
  static constexpr int kHeartbeatTimeoutSec = 30;
  static constexpr double kWorldTickIntervalSec = 1.0 / 30.0;

  GameServer(const std::string& ip, int port);
  ~GameServer();

  void Start();
  bool ListenOk() const { return !listen_failed_ && !startup_failed_; }
  void Loop();
  void Stop();

  void RunInLoop(std::function<void()> fn);

  void CheckHeartbeatTimeoutForTest() { CheckHeartbeatTimeout(); }

  void RegisterHandler(uint32_t msg_id, std::unique_ptr<IHandler> handler);
  IHandler* FindHandler(uint32_t msg_id) const;

  template <typename ReqMsg>
  void RegisterTypedHandler(uint32_t msg_id, IHandler* handler) {
    handler->SetServer(this);
    handlers_[msg_id] = handler;
    if constexpr (!std::is_same_v<ReqMsg, void>) {
      proto_factories_[msg_id] = []() -> std::shared_ptr<::google::protobuf::Message> {
        return std::make_shared<ReqMsg>();
      };
    }
  }

  void SendFrame(const ::zrpc::TcpConnectionPtr& conn, uint32_t msg_id,
                 const std::string& body, uint8_t* seq,
                 const std::string& proto_name = "");

  template <typename Msg>
  void SendMsg(const ::zrpc::TcpConnectionPtr& conn, uint32_t msg_id,
               const Msg& msg, uint8_t* seq) {
    std::string body;
    if (!msg.SerializeToString(&body)) {
      LOG_WARN << "SendMsg SerializeToString failed, msg_id=" << msg_id;
      return;
    }
    SendFrame(conn, msg_id, body, seq);
  }

  void SendMsg(const ::zrpc::TcpConnectionPtr& conn, uint32_t msg_id,
               const std::shared_ptr<::google::protobuf::Message>& msg,
               uint8_t* seq) {
    if (!msg) return;
    std::string body;
    if (!msg->SerializeToString(&body)) {
      LOG_WARN << "SendMsg(Message) SerializeToString failed, msg_id=" << msg_id;
      return;
    }
    SendFrame(conn, msg_id, body, seq);
  }

  void SendMsg(const ::zrpc::TcpConnectionPtr& conn, uint32_t msg_id,
               const std::string& body, uint8_t* seq) {
    SendFrame(conn, msg_id, body, seq);
  }

  WorldSystem* GetWorld() { return world_.get(); }
  AoiViewBridge* GetAoiBridge() { return aoi_bridge_.get(); }
  MapViewBridge* GetMapBridge() { return map_bridge_.get(); }
  JoltServer* GetJoltServer() { return jolt_server_.get(); }
  game::navigation::NavSystem* GetNavSystem() { return nav_system_.get(); }
  game::navigation::HeightMapSystem* GetHeightMapSystem() {
    return height_map_system_.get();
  }
  bool NavigationEnabled() const { return navigation_enabled_; }
  bool HeightMapValidationEnabled() const {
    return height_map_validation_enabled_;
  }
  float NavmeshMaxHorizontalError() const {
    return navmesh_max_horizontal_error_;
  }
  PlayerPersistSystem* GetPlayerPersist() { return player_persist_.get(); }
  mongo::PlayerMongoStorage* GetPlayerStorage() { return player_storage_.get(); }

  std::shared_ptr<PlayerArchiveService> GetPlayerArchive() {
    return player_archive_;
  }

  uint64_t GenSessionId() { return next_session_id_.fetch_add(1); }
  uint64_t GenRoleId() { return next_role_id_.fetch_add(1); }

  std::string GateAddr() const { return ip_ + ":" + std::to_string(port_); }
  void KickAndShutdownConnection(
      const EntityPtr& entity, uint32_t code_id, const char* reason,
      const ::zrpc::TcpConnectionPtr& except_conn = nullptr);

 private:
  void OnConnection(const ::zrpc::TcpConnectionPtr& conn);
  void OnMessage(const ::zrpc::TcpConnectionPtr& conn, ::zrpc::Buffer* buf);

  std::shared_ptr<::google::protobuf::Message> CreateRequest(
      uint32_t msg_id) const;

  void LoadConfigs();
  bool InitNavigation();
  void InitWorldAndAoi();
  void RegisterAllHandlers();
  void InitMongoIfEnabled();
  void InitServerNetwork();
  void StartTimers();

  void CheckHeartbeatTimeout();
  int HeartbeatTimeoutSec() const;
  int HeartbeatCheckIntervalSec() const;
  void DoGracefulStop();

  ::zrpc::EventLoop loop_;
  ::zrpc::TcpServer server_;
  std::string ip_;
  int port_ = 0;
  bool listen_failed_ = false;
  bool startup_failed_ = false;
  std::unordered_map<uint32_t, IHandler*> handlers_;
  std::unordered_set<std::unique_ptr<IHandler>> handler_owners_;
  std::unordered_map<
      uint32_t, std::function<std::shared_ptr<::google::protobuf::Message>()>>
      proto_factories_;
  std::atomic<uint64_t> next_session_id_{server::kSessionIdStart};
  std::atomic<uint64_t> next_role_id_{server::kRoleIdStart};
  std::atomic<bool> stopping_{false};

  std::shared_ptr<WorldSystem> world_;
  std::unique_ptr<AoiViewBridge> aoi_bridge_;
  std::unique_ptr<MapViewBridge> map_bridge_;
  std::unique_ptr<JoltServer> jolt_server_;
  std::unique_ptr<game::navigation::NavSystem> nav_system_;
  std::unique_ptr<game::navigation::HeightMapSystem> height_map_system_;
  bool navigation_enabled_ = true;
  bool height_map_validation_enabled_ = false;
  float navmesh_max_horizontal_error_ = 0.5f;

  std::unique_ptr<mongo::PlayerMongoStorage> player_storage_;
  std::unique_ptr<PlayerPersistSystem> player_persist_;
  std::shared_ptr<PlayerArchiveService> player_archive_;

  std::shared_ptr<::zrpc::Timer> heartbeat_timer_;
  std::shared_ptr<::zrpc::Timer> world_tick_timer_;
  std::shared_ptr<::zrpc::Timer> persist_timer_;
  std::shared_ptr<::zrpc::Timer> mongo_ping_timer_;
};
