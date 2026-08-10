// GameServer 启动：配置 / 世界 / Handler / Mongo / 网络 / 定时器

#include "game_server.h"

#include <cstdlib>
#include <cmath>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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
#include "navigation/nav_system.h"
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

bool EnvBool(const char* name, bool def) {
  const char* value = std::getenv(name);
  if (!value || !*value) {
    return def;
  }
  if (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 ||
      std::strcmp(value, "on") == 0) {
    return true;
  }
  if (std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0 ||
      std::strcmp(value, "off") == 0) {
    return false;
  }
  LOG_WARN << "invalid boolean environment value, use default name=" << name
           << " value=" << value;
  return def;
}

float EnvPositiveFloat(const char* name, float def, float maximum) {
  const char* value = std::getenv(name);
  if (!value || !*value) {
    return def;
  }
  char* end = nullptr;
  const float parsed = std::strtof(value, &end);
  if (end == value || *end != '\0' || !std::isfinite(parsed) ||
      parsed <= 0.0f || parsed > maximum) {
    LOG_WARN << "invalid positive float environment value, use default name="
             << name << " value=" << value;
    return def;
  }
  return parsed;
}

std::filesystem::path ResolveNavigationResource(
    const char* explicit_file_env,
    const char* directory_env,
    const std::string& res_id,
    const char* extension,
    bool allow_explicit_file) {
  if (allow_explicit_file) {
    if (const char* file = std::getenv(explicit_file_env); file && *file) {
      return std::filesystem::path(file);
    }
  }

  const std::string filename = res_id + extension;
  if (const char* directory = std::getenv(directory_env);
      directory && *directory) {
    return std::filesystem::path(directory) / filename;
  }

  const std::vector<std::filesystem::path> candidates = {
      std::filesystem::path("deps/map_res") / filename,
      std::filesystem::path("../deps/map_res") / filename,
  };
  std::error_code error;
  for (const auto& candidate : candidates) {
    if (std::filesystem::is_regular_file(candidate, error) && !error) {
      return candidate;
    }
    error.clear();
  }
  return candidates.front();
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

bool GameServer::InitNavigation() {
  navigation_enabled_ = EnvBool("GAME_NAVMESH_ENABLE", true);
  height_map_validation_enabled_ =
      EnvBool("GAME_HEIGHTMAP_VALIDATION_ENABLE", false);
  navmesh_max_horizontal_error_ = EnvPositiveFloat(
      "GAME_NAVMESH_MAX_HORIZONTAL_ERROR", 0.5f, 5.0f);

  if (!navigation_enabled_) {
    LOG_ERROR << "NavMesh validation is disabled by GAME_NAVMESH_ENABLE=0";
    nav_system_.reset();
    height_map_system_.reset();
    height_map_validation_enabled_ = false;
    return true;
  }

  nav_system_ = std::make_unique<game::navigation::NavSystem>();
  const MapConfig* default_map = MapConfigSystem::Instance().GetFirstMap();
  std::size_t loaded_count = 0;
  for (const uint32_t cfg_id : MapConfigSystem::Instance().GetAllCfgIds()) {
    const MapConfig* config = MapConfigSystem::Instance().Find(cfg_id);
    if (!config || config->res_id_.empty()) {
      LOG_ERROR << "NavMesh skipped invalid map config cfg_id=" << cfg_id;
      continue;
    }

    const bool is_default = default_map && config->cfg_id_ == default_map->cfg_id_;
    const auto path = ResolveNavigationResource(
        "GAME_NAVMESH_FILE", "GAME_NAVMESH_DIR", config->res_id_,
        ".navmesh", is_default);
    const auto status = nav_system_->load_navmesh(config->cfg_id_, path);
    if (status != game::navigation::NavStatus::success) {
      LOG_WARN << "NavMesh load failed map_cfg_id=" << config->cfg_id_
               << " path=" << path.string()
               << " status=" << game::navigation::to_string(status);
      continue;
    }
    ++loaded_count;
    LOG_INFO << "NavMesh loaded map_cfg_id=" << config->cfg_id_
             << " path=" << path.string();
  }

  const bool default_loaded =
      default_map && nav_system_->is_loaded(default_map->cfg_id_);
  if (!default_loaded && EnvBool("GAME_NAVMESH_REQUIRED", true)) {
    LOG_ERROR << "required default-map NavMesh is unavailable";
    return false;
  }
  LOG_INFO << "NavMesh initialization complete loaded_maps=" << loaded_count
           << " max_horizontal_error=" << navmesh_max_horizontal_error_;

  if (!height_map_validation_enabled_) {
    // 高度图验证代码已接好，当前默认关闭；稳定后通过环境变量启用。
    LOG_INFO << "height-map validation disabled "
             << "(GAME_HEIGHTMAP_VALIDATION_ENABLE=0)";
    height_map_system_.reset();
    return true;
  }

  height_map_system_ = std::make_unique<game::navigation::HeightMapSystem>();
  std::size_t height_map_count = 0;
  for (const uint32_t cfg_id : MapConfigSystem::Instance().GetAllCfgIds()) {
    const MapConfig* config = MapConfigSystem::Instance().Find(cfg_id);
    if (!config || config->res_id_.empty()) {
      continue;
    }
    const bool is_default = default_map && config->cfg_id_ == default_map->cfg_id_;
    const auto path = ResolveNavigationResource(
        "GAME_HEIGHTMAP_FILE", "GAME_HEIGHTMAP_DIR", config->res_id_,
        ".hgt", is_default);
    const auto status = height_map_system_->load(config->cfg_id_, path);
    if (status != game::navigation::NavStatus::success) {
      LOG_WARN << "height map load failed map_cfg_id=" << config->cfg_id_
               << " path=" << path.string()
               << " status=" << game::navigation::to_string(status);
      continue;
    }
    ++height_map_count;
  }
  if (!default_map || !height_map_system_->is_loaded(default_map->cfg_id_)) {
    LOG_ERROR << "height-map validation enabled but default resource missing";
    return false;
  }
  LOG_INFO << "height-map initialization complete loaded_maps="
           << height_map_count;
  return true;
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
