// GameServer 启动：配置 / 世界 / Handler / Mongo / 网络 / 定时器

#include "game_server.h"

#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <string>

#include "MongoConfig.h"
#include "PlayerMongoStorage.h"
#include "client_3d.pb.h"
#include "client_login.pb.h"
#include "common/event_bus.h"
#include "common/init.h"
#include "common/player_config.h"
#include "ecs/components/connection_component.h"
#include "ecs/systems/map_config_system.h"
#include "ecs/systems/player_persist_system.h"
#include "handlers/account_handler.h"
#include "handlers/connection_handler.h"
#include "handlers/game_handler.h"
#include "handlers/jump_handler.h"
#include "handlers/move_handler.h"
#include "handlers/role_handler.h"
#include "protocol/pack_flags.h"
#include "server_defaults.h"
#include "utils/map_util.h"
#include "zrpc/base/logger.h"

namespace {

int EnvPositiveInt(const char* name, int def) {
  const char* v = std::getenv(name);
  if (!v || !*v) {
    return def;
  }
  const int n = std::atoi(v);
  return n > 0 ? n : def;
}

bool MongoEnabledByEnv() {
  const char* e = std::getenv("GAME_MONGO_ENABLE");
  if (e && *e) {
    if (std::strcmp(e, "1") == 0) {
      return true;
    }
    if (std::strcmp(e, "0") == 0) {
      return false;
    }
  }
  return false;
}

int MongoPersistIntervalSec() {
  const char* v = std::getenv("GAME_MONGO_PERSIST_INTERVAL_SEC");
  if (!v || !*v) {
    return 5;
  }
  const int n = std::atoi(v);
  return n > 0 ? n : 5;
}

int MongoPersistBatch() {
  const char* v = std::getenv("GAME_MONGO_PERSIST_BATCH");
  if (!v || !*v) {
    return 100;
  }
  const int n = std::atoi(v);
  return n > 0 ? n : 100;
}

}  // namespace

int GameServer::HeartbeatTimeoutSec() const {
  return EnvPositiveInt("GAME_HEARTBEAT_TIMEOUT_SEC", kHeartbeatTimeoutSec);
}

int GameServer::HeartbeatCheckIntervalSec() const {
  return EnvPositiveInt("GAME_HEARTBEAT_CHECK_INTERVAL_SEC",
                        kHeartbeatCheckIntervalSec);
}

void GameServer::LoadConfigs() {
  MapConfigSystem::Instance().LoadDefaults();
  PlayerConfig::Instance().LoadDefaults();
}

void GameServer::InitWorldAndAoi() {
  world_ = WorldSystem::Create(SceneRegionType::kMap);
  world_->Init();

  aoi_bridge_ = std::make_unique<AoiViewBridge>(
      world_.get(),
      [weak = weak_from_this()](uint64_t conn_owner_id, uint32_t msg_id,
                                const std::string& body) {
        auto self = weak.lock();
        if (!self) {
          return;
        }
        auto owner = self->world_->FindEntity(conn_owner_id);
        if (!owner) {
          return;
        }
        auto* cc = owner->GetComponent<ConnectionComponent>();
        if (!cc || !cc->conn_ || !cc->conn_->Connected()) {
          return;
        }
        self->SendMsg(cc->conn_, msg_id, body, &cc->send_seq_);
      });
  aoi_bridge_->Install();

  map_bridge_ = std::make_unique<MapViewBridge>(
      world_.get(),
      [weak = weak_from_this()](uint64_t conn_owner_id, uint32_t msg_id,
                                const std::string& body) {
        auto self = weak.lock();
        if (!self) {
          return;
        }
        auto owner = self->world_->FindEntity(conn_owner_id);
        if (!owner) {
          return;
        }
        auto* cc = owner->GetComponent<ConnectionComponent>();
        if (!cc || !cc->conn_ || !cc->conn_->Connected()) {
          return;
        }
        self->SendMsg(cc->conn_, msg_id, body, &cc->send_seq_);
      });
  map_bridge_->Install();

  jolt_server_ = std::make_unique<JoltServer>();
  if (jolt_server_->Init()) {
    std::string res_id;
    if (const auto* first = MapConfigSystem::Instance().GetFirstMap()) {
      res_id = first->res_id_;
    }
    const std::string obj_path = server::ResolveMapObjPath(res_id);
    if (jolt_server_->LoadMap(obj_path)) {
      const auto& b = jolt_server_->GetBounds();
      LOG_INFO << "=== Map Bounds ===";
      LOG_INFO << "  min=(" << b.min.GetX() << "," << b.min.GetY() << ","
               << b.min.GetZ() << ")";
      LOG_INFO << "  max=(" << b.max.GetX() << "," << b.max.GetY() << ","
               << b.max.GetZ() << ")";
      LOG_INFO << "  center=(" << b.center.GetX() << "," << b.center.GetY()
               << "," << b.center.GetZ() << ")";
      LOG_INFO << "  size=(" << b.size.GetX() << "," << b.size.GetY() << ","
               << b.size.GetZ() << ")";
      LOG_INFO << "==================";

      world_->SetBoundsClamp(
          [js = jolt_server_.get()](const Vector3D& pos) -> Vector3D {
            if (!js || !js->IsMapLoaded()) return pos;
            return server::ClampToMapBounds(js->GetBounds(), pos);
          });
    } else {
      LOG_WARN << "JoltServer LoadMap failed, running without physics bounds"
               << " path=" << obj_path;
    }
  } else {
    LOG_ERROR << "JoltServer Init failed";
    jolt_server_.reset();
  }

  if (jolt_server_) {
    world_->Jolt().Bind(jolt_server_.get(),
                        static_cast<float>(kWorldTickIntervalSec));
    world_->SetJoltServer(jolt_server_.get());
    LOG_INFO << "JoltSystem bound to WorldSystem";
  }
}

void GameServer::RegisterAllHandlers() {
  auto add = [weak = weak_from_this()](std::unique_ptr<IHandler> h) {
    auto self = weak.lock();
    if (!self) {
      return static_cast<IHandler*>(nullptr);
    }
    auto* raw = h.get();
    self->handler_owners_.insert(std::move(h));
    return raw;
  };

  auto* conn = add(std::make_unique<ConnectionHandler>());
  RegisterTypedHandler<::cli_handshake_req>(proto_id("cli_handshake_req"),
                                            conn);
  RegisterTypedHandler<void>(proto_id("cli_heart_beat_req"), conn);
  RegisterTypedHandler<::cli_reconnect_req>(proto_id("cli_reconnect_req"),
                                            conn);

  auto* acct = add(std::make_unique<AccountHandler>());
  RegisterTypedHandler<::cli_user_login_req>(proto_id("cli_user_login_req"),
                                             acct);

  auto* role = add(std::make_unique<RoleHandler>());
  RegisterTypedHandler<::cli_role_list_req>(proto_id("cli_role_list_req"),
                                            role);
  RegisterTypedHandler<::cli_role_create_req>(proto_id("cli_role_create_req"),
                                              role);
  RegisterTypedHandler<::cli_role_delete_req>(proto_id("cli_role_delete_req"),
                                              role);
  RegisterTypedHandler<::cli_role_login_req>(proto_id("cli_role_login_req"),
                                             role);
  RegisterTypedHandler<::cli_random_name_req>(proto_id("cli_random_name_req"),
                                              role);

  auto* game = add(std::make_unique<GameHandler>());
  RegisterTypedHandler<void>(proto_id("cli_enter_game_req"), game);

  auto* move = add(std::make_unique<MoveHandler>());
  RegisterTypedHandler<::cli_3d_move_req>(proto_id("cli_3d_move_req"), move);

  auto* jump = static_cast<JumpHandler*>(add(std::make_unique<JumpHandler>()));
  RegisterTypedHandler<::cli_3d_jump_req>(proto_id("cli_3d_jump_req"), jump);
  // EventBus 为进程单例：必须 weak 守护，避免测试多次启停后悬空 JumpHandler*
  EventBus::Instance().Subscribe<EvtLeaveMap>(
      [weak = weak_from_this(), jump](const EvtLeaveMap& ev) {
        auto self = weak.lock();
        if (!self || !ev.entity) {
          return;
        }
        jump->ClearCooldown(ev.entity);
      });

  LOG_INFO << "all handlers registered, count=" << handlers_.size();
  for (const auto& [msg_id, _] : handlers_) {
    LOG_INFO << "  registered: " << msg_id_name(msg_id) << " msg_id=" << msg_id;
  }
}

void GameServer::InitMongoIfEnabled() {
  if (!MongoEnabledByEnv()) {
    LOG_INFO << "mongo player storage disabled (GAME_MONGO_ENABLE not set)";
    return;
  }
  try {
    auto config = mongo::MongoConfig::FromEnvironment();
    mongo::PlayerMongoStorageOptions opts;
    if (const char* v = std::getenv("GAME_MONGO_PLAYERS_COLLECTION")) {
      if (*v != '\0') {
        opts.collection = v;
      }
    }
    if (const char* v = std::getenv("GAME_MONGO_ACCOUNT_INFO_COLLECTION")) {
      if (*v != '\0') {
        opts.accountInfoCollection = v;
      }
    }
    auto storage = std::make_unique<mongo::PlayerMongoStorage>(
        std::move(config), std::move(opts),
        [](std::int64_t playerId, std::exception_ptr ep) {
          try {
            if (ep) {
              std::rethrow_exception(ep);
            }
          } catch (const std::exception& e) {
            LOG_WARN << "player persist error, role_id=" << playerId
                     << " what=" << e.what();
          }
        });
    auto persist = std::make_unique<PlayerPersistSystem>();
    persist->SetStorage(storage.get());
    persist->SetPostToLoop([weak = weak_from_this()](std::function<void()> fn) {
      if (auto self = weak.lock()) {
        self->RunInLoop(std::move(fn));
      }
    });
    player_storage_ = std::move(storage);
    player_persist_ = std::move(persist);
    player_archive_ =
        std::make_shared<PlayerArchiveService>(loop_, player_storage_.get());
    LOG_INFO << "mongo player storage enabled, persist_interval_sec="
             << MongoPersistIntervalSec() << " batch=" << MongoPersistBatch()
             << " players_collection=" << player_storage_->PlayersCollection()
             << " account_info_collection="
             << player_storage_->AccountInfoCollection();
  } catch (const std::exception& e) {
    LOG_ERROR << "mongo player storage init failed, disabled: " << e.what();
    player_storage_.reset();
    player_persist_.reset();
    player_archive_.reset();
  }
}

void GameServer::InitServerNetwork() {
  server_.SetThreadNum(0);
  server_.SetConnectionCallback(
      [weak = weak_from_this()](const ::zrpc::TcpConnectionPtr& conn) {
        if (auto self = weak.lock()) {
          self->OnConnection(conn);
        }
      });
  server_.SetMessageCallback(
      [weak = weak_from_this()](const ::zrpc::TcpConnectionPtr& conn,
                                ::zrpc::Buffer* buf) {
        if (auto self = weak.lock()) {
          self->OnMessage(conn, buf);
        }
      });
  if (!server_.Start()) {
    listen_failed_ = true;
    LOG_ERROR << "FATAL: server_.Start() failed — port in use or bind error on "
              << ip_ << ":" << port_;
    return;
  }
  listen_failed_ = false;
  LOG_INFO << "svc_game_3d_server listening on port=" << port_;
}

void GameServer::StartTimers() {
  heartbeat_timer_ = loop_.RunAfter(
      HeartbeatCheckIntervalSec(), true, [weak = weak_from_this()]() {
        if (auto self = weak.lock()) {
          self->CheckHeartbeatTimeout();
        }
      });
  LOG_INFO << "heartbeat timeout checker started, interval="
           << HeartbeatCheckIntervalSec()
           << "s timeout=" << HeartbeatTimeoutSec() << "s";

  world_tick_timer_ = loop_.RunAfter(
      kWorldTickIntervalSec, true, [weak = weak_from_this()]() {
        auto self = weak.lock();
        if (!self) {
          return;
        }
        if (self->world_) {
          self->world_->Tick(static_cast<float>(kWorldTickIntervalSec));
        }
        if (IsStopRequested() && !self->stopping_.exchange(true)) {
          self->DoGracefulStop();
        }
      });
  LOG_INFO << "world tick started, interval=" << kWorldTickIntervalSec
           << "s (move + jolt + aoi unified)";

  persist_timer_ = loop_.RunAfter(
      MongoPersistIntervalSec(), true, [weak = weak_from_this()]() {
        auto self = weak.lock();
        if (!self) {
          return;
        }
        if (self->player_persist_) {
          self->player_persist_->TickPersist(MongoPersistBatch());
        }
      });
  LOG_INFO << "player persist timer started, interval="
           << MongoPersistIntervalSec() << "s batch=" << MongoPersistBatch();

  if (player_storage_) {
    constexpr int kMongoPingIntervalSec = 120;
    mongo_ping_timer_ = loop_.RunAfter(
        kMongoPingIntervalSec, true, [weak = weak_from_this()]() {
          auto self = weak.lock();
          if (!self) {
            return;
          }
          if (self->player_storage_) {
            self->player_storage_->PostPing();
          }
        });
    LOG_INFO << "mongo ping timer started, interval=" << kMongoPingIntervalSec
             << "s";
  }
}
