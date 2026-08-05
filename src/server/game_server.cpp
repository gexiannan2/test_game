#include "game_server.h"

#include <algorithm>
#include <any>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <vector>

#include "client_3d.pb.h"
#include "client_login.pb.h"
#include "common/init.h"
#include "ecs/component_base/component_base.h"
#include "ecs/components/account_component.h"
#include "ecs/components/connection_component.h"
#include "ecs/components/map_component.h"
#include "ecs/components/role_component.h"
#include "ecs/components/transform_component.h"
#include "ecs/entity/entity.h"
#include "session/system.h"
#include "ecs/entity/player_entity.h"
#include "handlers/account_handler.h"
#include "handlers/connection_handler.h"
#include "handlers/game_handler.h"
#include "handlers/move_handler.h"
#include "handlers/role_handler.h"
#include "protocol/pack_flags.h"
#include "map_bounds_util.h"
#include "server_constants.h"
#include "zrpc/base/logger.h"
#include "zrpc/net/socket.h"
#include "zrpc/net/tcp_connection.h"

#include <exception>

#include "MongoConfig.h"
#include "MongoError.h"
#include "PlayerMongoStorage.h"
#include "ecs/systems/player_persist_system.h"

namespace
{
    int EnvPositiveInt(const char* name, int def)
    {
        const char* v = std::getenv(name);
        if (!v || !*v)
        {
            return def;
        }
        const int n = std::atoi(v);
        return n > 0 ? n : def;
    }

    // 启用策略：默认不连接 mongo；只有 GAME_MONGO_ENABLE=1 才显式启用。
    bool MongoEnabledByEnv()
    {
        const char* e = std::getenv("GAME_MONGO_ENABLE");
        if (e && *e)
        {
            if (std::strcmp(e, "1") == 0)
            {
                return true;
            }
            if (std::strcmp(e, "0") == 0)
            {
                return false;
            }
        }
        return false;
    }

    int MongoPersistIntervalSec()
    {
        const char* v = std::getenv("GAME_MONGO_PERSIST_INTERVAL_SEC");
        if (!v || !*v)
        {
            return 5;
        }
        const int n = std::atoi(v);
        return n > 0 ? n : 5;
    }

    int MongoPersistBatch()
    {
        const char* v = std::getenv("GAME_MONGO_PERSIST_BATCH");
        if (!v || !*v)
        {
            return 100;
        }
        const int n = std::atoi(v);
        return n > 0 ? n : 100;
    }

} // namespace

int GameServer::HeartbeatTimeoutSec() const
{
    return EnvPositiveInt("GAME_HEARTBEAT_TIMEOUT_SEC", kHeartbeatTimeoutSec);
}

int GameServer::HeartbeatCheckIntervalSec() const
{
    return EnvPositiveInt("GAME_HEARTBEAT_CHECK_INTERVAL_SEC",
                          kHeartbeatCheckIntervalSec);
}

GameServer::GameServer(const std::string& ip, int port)
    : server_(&loop_, ip, static_cast<int16_t>(port), nullptr),
      ip_(ip), port_(port)
{
}

GameServer::~GameServer()
{
    if (heartbeat_timer_)
    {
        loop_.CancelAfter(heartbeat_timer_);
        heartbeat_timer_.reset();
    }

    if (world_tick_timer_)
    {
        loop_.CancelAfter(world_tick_timer_);
        world_tick_timer_.reset();
    }

    if (persist_timer_)
    {
        loop_.CancelAfter(persist_timer_);
        persist_timer_.reset();
    }

    if (mongo_ping_timer_)
    {
        loop_.CancelAfter(mongo_ping_timer_);
        mongo_ping_timer_.reset();
    }
}

void GameServer::Start()
{
    LoadConfigs();
    InitWorldAndAoi();
    RegisterAllHandlers();
    InitMongoIfEnabled();
    InitServerNetwork();
    if (listen_failed_)
    {
        LOG_ERROR << "GameServer::Start aborted: listen failed on "
                  << ip_ << ":" << port_;
        return;
    }
    StartTimers();
}

// ---- 配置加载 ----
void GameServer::LoadConfigs()
{
    MapConfigSystem::Instance().LoadDefaults();
    PlayerConfig::Instance().LoadDefaults();
}

// ---- 世界 + AOI 桥 ----
void GameServer::InitWorldAndAoi()
{
    world_ = WorldSystem::Create(SceneRegionType::kMap);
    world_->Init();

    aoi_bridge_ = std::make_unique<AoiViewBridge>(
        world_.get(),
        [weak = weak_from_this()](uint64_t conn_owner_id, uint32_t msg_id, const std::string& body) {
            auto self = weak.lock();
            if (!self)
            {
                return;
            }
            auto owner = self->world_->FindEntity(conn_owner_id);
            if (!owner)
            {
                return;
            }
            auto* cc = owner->GetComponent<ConnectionComponent>();
            if (!cc || !cc->conn_ || !cc->conn_->Connected())
            {
                return;
            }
            self->SendMsg(cc->conn_, msg_id, body, &cc->send_seq_);
        });
    aoi_bridge_->Install();

    // Jolt 物理初始化 + 加载 OBJ 地图
    jolt_server_ = std::make_unique<JoltServer>();
    if (jolt_server_->Init())
    {
        const char* obj_env = std::getenv("GAME_MAP_OBJ");
        std::string obj_path = obj_env && *obj_env
            ? obj_env
            : "../deps/map_res/1001.obj";
        if (jolt_server_->LoadMap(obj_path))
        {
            const auto& b = jolt_server_->GetBounds();
            LOG_INFO << "=== Map Bounds ===";
            LOG_INFO << "  min=(" << b.min.GetX() << "," << b.min.GetY() << "," << b.min.GetZ() << ")";
            LOG_INFO << "  max=(" << b.max.GetX() << "," << b.max.GetY() << "," << b.max.GetZ() << ")";
            LOG_INFO << "  center=(" << b.center.GetX() << "," << b.center.GetY() << "," << b.center.GetZ() << ")";
            LOG_INFO << "  size=(" << b.size.GetX() << "," << b.size.GetY() << "," << b.size.GetZ() << ")";
            LOG_INFO << "==================";

            // 绑定边界钳制到 WorldSystem，所有 MoveEntity 调用均受保护
            world_->SetBoundsClamp(
                [js = jolt_server_.get()](const Vector3D& pos) -> Vector3D {
                    if (!js || !js->IsMapLoaded()) return pos;
                    return server::ClampToMapBounds(js->GetBounds(), pos);
                });
        }
        else
        {
            LOG_WARN << "JoltServer LoadMap failed, running without physics bounds";
        }
    }
    else
    {
        LOG_ERROR << "JoltServer Init failed";
        jolt_server_.reset();
    }

    // 绑定 JoltServer 到 WorldSystem 的 JoltSystem
    if (jolt_server_)
    {
        world_->Jolt().Bind(jolt_server_.get(), static_cast<float>(kWorldTickIntervalSec));
        world_->SetJoltServer(jolt_server_.get());
        LOG_INFO << "JoltSystem bound to WorldSystem";
    }
}

// ---- Handler 注册 ----
void GameServer::RegisterAllHandlers()
{
    auto add = [weak = weak_from_this()](std::unique_ptr<IHandler> h) {
        auto self = weak.lock();
        if (!self)
        {
            return static_cast<IHandler*>(nullptr);
        }
        auto* raw = h.get();
        self->handler_owners_.insert(std::move(h));
        return raw;
    };

    auto* conn = add(std::make_unique<ConnectionHandler>());
    RegisterTypedHandler<::cli_handshake_req>(proto_id("cli_handshake_req"), conn);
    RegisterTypedHandler<void>(proto_id("cli_heart_beat_req"), conn);
    RegisterTypedHandler<::cli_reconnect_req>(proto_id("cli_reconnect_req"), conn);

    auto* acct = add(std::make_unique<AccountHandler>());
    RegisterTypedHandler<::cli_user_login_req>(proto_id("cli_user_login_req"), acct);

    auto* role = add(std::make_unique<RoleHandler>());
    RegisterTypedHandler<::cli_role_list_req>(proto_id("cli_role_list_req"), role);
    RegisterTypedHandler<::cli_role_create_req>(proto_id("cli_role_create_req"), role);
    RegisterTypedHandler<::cli_role_delete_req>(proto_id("cli_role_delete_req"), role);
    RegisterTypedHandler<::cli_role_login_req>(proto_id("cli_role_login_req"), role);
    RegisterTypedHandler<::cli_random_name_req>(proto_id("cli_random_name_req"), role);

    auto* game = add(std::make_unique<GameHandler>());
    RegisterTypedHandler<void>(proto_id("cli_enter_game_req"), game);

    auto* move = add(std::make_unique<MoveHandler>());
    RegisterTypedHandler<::cli_3d_move_req>(proto_id("cli_3d_move_req"), move);

    LOG_INFO << "all handlers registered, count=" << handlers_.size();

    for (const auto& [msg_id, _] : handlers_)
    {
        LOG_INFO << "  registered: " << msg_id_name(msg_id) << " msg_id=" << msg_id;
    }
}

// ---- Mongo 存储 ----
void GameServer::InitMongoIfEnabled()
{
    if (!MongoEnabledByEnv())
    {
        LOG_INFO << "mongo player storage disabled (GAME_MONGO_ENABLE not set)";
        return;
    }
    try
    {
        auto config = mongo::MongoConfig::FromEnvironment();
        auto storage = std::make_unique<mongo::PlayerMongoStorage>(
            std::move(config),
            mongo::PlayerMongoStorageOptions{},
            [](std::int64_t playerId, std::exception_ptr ep) {
                try
                {
                    if (ep)
                    {
                        std::rethrow_exception(ep);
                    }
                }
                catch (const std::exception& e)
                {
                    LOG_WARN << "player persist error, role_id=" << playerId
                             << " what=" << e.what();
                }
            });
        auto persist = std::make_unique<PlayerPersistSystem>();
        persist->SetStorage(storage.get());
        persist->SetPostToLoop([weak = weak_from_this()](std::function<void()> fn) {
            if (auto self = weak.lock())
            {
                self->RunInLoop(std::move(fn));
            }
        });
        player_storage_ = std::move(storage);
        player_persist_ = std::move(persist);
        LOG_INFO << "mongo player storage enabled, persist_interval_sec="
                 << MongoPersistIntervalSec() << " batch=" << MongoPersistBatch();
    }
    catch (const std::exception& e)
    {
        LOG_ERROR << "mongo player storage init failed, disabled: " << e.what();
        player_storage_.reset();
        player_persist_.reset();
    }
}

// ---- 网络监听 ----
void GameServer::InitServerNetwork()
{
    server_.SetThreadNum(0);  // 单线程，业务与网络同环
    server_.SetConnectionCallback(
        [weak = weak_from_this()](const ::zrpc::TcpConnectionPtr& conn) {
            if (auto self = weak.lock())
            {
                self->OnConnection(conn);
            }
        });
    server_.SetMessageCallback(
        [weak = weak_from_this()](const ::zrpc::TcpConnectionPtr& conn,
                                   ::zrpc::Buffer* buf) {
            if (auto self = weak.lock())
            {
                self->OnMessage(conn, buf);
            }
        });
    if (!server_.Start())
    {
        listen_failed_ = true;
        LOG_ERROR << "FATAL: server_.Start() failed — port in use or bind error on "
                  << ip_ << ":" << port_;
        return;
    }
    listen_failed_ = false;
    LOG_INFO << "svc_game_3d_server listening on port=" << port_;
}

// ---- 定时器 ----
void GameServer::StartTimers()
{
    heartbeat_timer_ = loop_.RunAfter(
        HeartbeatCheckIntervalSec(), true,
        [weak = weak_from_this()]() {
            if (auto self = weak.lock())
            {
                self->CheckHeartbeatTimeout();
            }
        });
    LOG_INFO << "heartbeat timeout checker started, interval="
             << HeartbeatCheckIntervalSec() << "s timeout="
             << HeartbeatTimeoutSec() << "s";

    world_tick_timer_ = loop_.RunAfter(
        kWorldTickIntervalSec, true,
        [weak = weak_from_this()]() {
            auto self = weak.lock();
            if (!self)
            {
                return;
            }
            if (self->world_)
            {
                self->world_->Tick(static_cast<float>(kWorldTickIntervalSec));
            }
            if (IsStopRequested() && !self->stopping_.exchange(true))
            {
                self->DoGracefulStop();
            }
        });
    LOG_INFO << "world tick started, interval=" << kWorldTickIntervalSec
             << "s (move + jolt + aoi unified)";

    persist_timer_ = loop_.RunAfter(
        MongoPersistIntervalSec(), true,
        [weak = weak_from_this()]() {
            auto self = weak.lock();
            if (!self)
            {
                return;
            }
            if (self->player_persist_)
            {
                self->player_persist_->TickPersist(MongoPersistBatch());
            }
        });
    LOG_INFO << "player persist timer started, interval=" << MongoPersistIntervalSec()
             << "s batch=" << MongoPersistBatch();

    // mongo 连接保活：定期 ping，防止连接池空闲超时被 mongo 服务端断开。
    if (player_storage_)
    {
        constexpr int kMongoPingIntervalSec = 120;
        mongo_ping_timer_ = loop_.RunAfter(
            kMongoPingIntervalSec, true,
            [weak = weak_from_this()]() {
                auto self = weak.lock();
                if (!self)
                {
                    return;
                }
                if (self->player_storage_)
                {
                    self->player_storage_->PostPing();
                }
            });
        LOG_INFO << "mongo ping timer started, interval=" << kMongoPingIntervalSec << "s";
    }
}

void GameServer::DoGracefulStop()
{
    LOG_INFO << "graceful shutdown requested, draining all connections...";
    // 1. 取消周期定时器，停止心跳扫描与 world tick（drain 期间不再产生新事件）
    if (heartbeat_timer_)
    {
        loop_.CancelAfter(heartbeat_timer_);
        heartbeat_timer_.reset();
    }

    if (world_tick_timer_)
    {
        loop_.CancelAfter(world_tick_timer_);
        world_tick_timer_.reset();
    }

    if (persist_timer_)
    {
        loop_.CancelAfter(persist_timer_);
        persist_timer_.reset();
    }

    if (mongo_ping_timer_)
    {
        loop_.CancelAfter(mongo_ping_timer_);
        mongo_ping_timer_.reset();
    }

    // 2. 主动让所有在线玩家离图：此时连接仍 Connected，disappear 能发出，
    //    邻居先收到消失再断连。server_.Stop 的 ForceClose 也会经 OnConnection(false)
    //    走 LeaveMap，但此时 IsInMap 已 false 会短路，不会重复。
    if (world_)
    {
        const auto all_players = PlayerEntitySystem::Instance().GetAllByUidSnapshot();
        for (const auto& [uid, entity] : all_players)
        {
            (void)uid;
            if (entity && entity->IsInMap())
            {
                world_->LeaveMap(entity);
            }
        }
    }
    // 排空已入队的玩家快照，尽量不丢最后几笔写（阻塞 loop 线程最多 5s）。
    if (player_persist_)
    {
        player_persist_->FlushOnShutdown();
    }
    // 3. server_.Stop 异步 ForceClose 所有连接（触发 OnConnection(false) 清理），
    //    连接全部移除后回调里 Quit。期间 loop 继续跑处理断连回调，直至清空。
    //    不在此处立即 Quit——否则 ForceClose 的 QueueInLoop / 0.01s 重试定时器
    //    无法执行，连接与线程池将残留。
    server_.Stop([weak = weak_from_this()]() {
        auto self = weak.lock();
        if (!self)
        {
            return;
        }
        LOG_INFO << "all connections drained, tcp server stopped, quit loop";
        self->loop_.Quit();
    });
}

void GameServer::Loop()
{
    loop_.Run();
}

void GameServer::Stop()
{
    loop_.RunInLoop([weak = weak_from_this()] {
        auto self = weak.lock();
        if (!self)
        {
            return;
        }
        if (!self->stopping_.exchange(true))
        {
            self->DoGracefulStop();
        }
    });
}

void GameServer::RunInLoop(std::function<void()> fn)
{
    loop_.RunInLoop(std::move(fn));
}

void GameServer::PostAccountInfoAfterLogin(const EntityPtr& entity)
{
    if (!player_storage_ || !entity)
    {
        return;
    }

    const auto* account = entity->GetComponent<AccountComponent>();
    if (!account)
    {
        LOG_WARN << "account_info skipped: no AccountComponent, entity="
                 << entity->GetId();
        return;
    }
    try
    {
        if (!mongo::IsValidAccountInfoAccount(account->uid_))
        {
            LOG_WARN << "account_info skipped: account must be valid UTF-8 with 1-20 characters"
                     << " account_bytes=" << account->uid_.size()
                     << " channel_id=" << account->channel_id_;
            return;
        }

        mongo::AccountInfoSnapshot snapshot;
        snapshot.account = account->uid_;
        snapshot.channelId = account->channel_id_;
        const auto accountBytes = snapshot.account.size();
        const auto channelId = snapshot.channelId;

        player_storage_->PostUpsertAccountInfo(
            std::move(snapshot),
            [accountBytes, channelId](bool success, std::string, std::exception_ptr ep)
            {
                if (success)
                {
                    return;
                }
                if (!ep)
                {
                    LOG_WARN << "account_info upsert rejected: queue full or stopping"
                             << " account_bytes=" << accountBytes
                             << " channel_id=" << channelId
                             << " area_id=0";
                    return;
                }
                try
                {
                    std::rethrow_exception(ep);
                }
                catch (const mongo::MongoError& error)
                {
                    LOG_WARN << "account_info upsert failed"
                             << " account_bytes=" << accountBytes
                             << " channel_id=" << channelId
                             << " area_id=0"
                             << " op=" << error.operation()
                             << " code=" << error.code()
                             << " what=" << error.what();
                }
                catch (const std::exception& error)
                {
                    LOG_WARN << "account_info upsert failed"
                             << " account_bytes=" << accountBytes
                             << " channel_id=" << channelId
                             << " area_id=0"
                             << " what=" << error.what();
                }
            });
    }
    catch (const std::exception& error)
    {
        LOG_WARN << "account_info submit failed before enqueue"
                 << " account_bytes=" << account->uid_.size()
                 << " channel_id=" << account->channel_id_
                 << " area_id=0"
                 << " what=" << error.what();
    }
    catch (...)
    {
        LOG_WARN << "account_info submit failed before enqueue"
                 << " account_bytes=" << account->uid_.size()
                 << " channel_id=" << account->channel_id_
                 << " area_id=0 unknown_error";
    }
}

void GameServer::QueryRole(uint64_t roleId, std::function<void(bool, ::entity_player_data)> cb)
{
    if (!player_storage_)
    {
        RunInLoop([cb = std::move(cb)]()
        {
            cb(false, {});
        });
        return;
    }
    player_storage_->PostLoad(static_cast<std::int64_t>(roleId),
        [weak = weak_from_this(), roleId, cb = std::move(cb)](bool success, std::int64_t,
                                                                ::entity_player_data data,
                                                                std::exception_ptr ep)
        {
            auto self = weak.lock();
            if (!self)
            {
                return;
            }
            self->RunInLoop([cb = std::move(cb), success, data = std::move(data), ep, roleId]()
            {
                if (ep)
                {
                    try
                    {
                        std::rethrow_exception(ep);
                    }
                    catch (const std::exception& e)
                    {
                        LOG_WARN << "QueryRole failed role_id=" << roleId << " what=" << e.what();
                    }
                }
                cb(success, std::move(data));
            });
        });
}

void GameServer::DeleteRoleArchive(uint64_t roleId, std::function<void(bool)> cb)
{
    if (!player_storage_)
    {
        if (cb)
        {
            RunInLoop([cb = std::move(cb)]()
            {
                cb(false);
            });
        }
        return;
    }
    player_storage_->PostDelete(static_cast<std::int64_t>(roleId),
        [weak = weak_from_this(), roleId, cb = std::move(cb)](bool success, std::int64_t,
                                                                std::exception_ptr ep)
        {
            auto self = weak.lock();
            if (!self)
            {
                return;
            }
            self->RunInLoop([cb = std::move(cb), success, ep, roleId]()
            {
                if (ep)
                {
                    try
                    {
                        std::rethrow_exception(ep);
                    }
                    catch (const std::exception& e)
                    {
                        LOG_WARN << "DeleteRoleArchive failed role_id=" << roleId << " what=" << e.what();
                    }
                }
                if (cb)
                {
                    cb(success);
                }
            });
        });
}

void GameServer::RegisterHandler(uint32_t msg_id,
                                  std::unique_ptr<IHandler> handler)
{
    handler->SetServer(this);
    handlers_[msg_id] = handler.get();
    handler_owners_.insert(std::move(handler));
}

IHandler* GameServer::FindHandler(uint32_t msg_id) const
{
    auto it = handlers_.find(msg_id);
    if (it == handlers_.end())
    {
        return nullptr;
    }
    return it->second;
}

void GameServer::OnConnection(const ::zrpc::TcpConnectionPtr& conn)
{
    if (conn->Connected())
    {
        ::zrpc::socket::SetKeepAlive(conn->GetSockfd(), 1);
        // 临时实体；登录时可能被缓存实体顶替
        auto player = std::make_shared<PlayerEntity>(world_->AllocateEntityId());
        player->AddComponent<ConnectionComponent>();
        player->GetComponent<ConnectionComponent>()->conn_ = conn;
        conn->SetContext(EntityPtr(player));
        LOG_INFO << "client connected, player=" << player->GetId();
    }
    else
    {
        auto* any_ptr = std::any_cast<EntityPtr>(&conn->GetContext());
        if (!any_ptr || !(*any_ptr))
        {
            LOG_WARN << "disconnect with no entity context";
            return;
        }
        auto entity = *any_ptr;

        // 顶号/重连后实体已挂新连接，旧连接异步 disconnect 不得再 LeaveMap
        auto* cc = entity->GetComponent<ConnectionComponent>();
        if (cc && cc->conn_ && cc->conn_ != conn)
        {
            LOG_INFO << "stale disconnect ignored, entity=" << entity->GetId()
                     << " owned_by_other_conn";
            return;
        }

        entity->SetState(Entity::State::kDisconnected);

        // 先掐断发包绑定，避免后续 AOI 广播再往本连接 Send
        if (cc)
        {
            cc->conn_.reset();
        }

        if (entity->IsInMap())
        {
            world_->LeaveMap(entity);
        }
        entity->RemoveComponent<ConnectionComponent>();
        // 下线 flush：立即落地最后位置（不等 tick），保证下线不丢。
        if (player_persist_)
        {
            auto* role = entity->GetComponent<RoleComponent>();
            if (role && role->role_id_ != 0)
            {
                player_persist_->FlushPlayer(role->role_id_);
            }
        }
        LOG_INFO << "client disconnected, entity cached: id=" << entity->GetId()
                 << " cached_players="
                 << PlayerEntitySystem::Instance().GetPlayerCount();
    }
}

void GameServer::OnMessage(const ::zrpc::TcpConnectionPtr& conn,
                            ::zrpc::Buffer* buf)
{
    std::vector<PackFrame> frames;
    if (!TryDecodeFrames(buf, frames))
    {
        const int32_t len = buf->ReadableBytes();
        const char* data = buf->Peek();
        const int32_t dump_len = (len < 80) ? len : 80;
        std::ostringstream hex;
        hex << "len=" << len << "B raw(" << dump_len << "B): ";
        for (int32_t i = 0; i < dump_len; ++i)
        {
            hex << std::hex << std::setw(2) << std::setfill('0')
                << (static_cast<unsigned char>(data[i]) & 0xff) << " ";
        }
        LOG_WARN << "protocol error (bad frame header/magic), close connection, "
                 << hex.str();
        conn->Shutdown();
        return;
    }

    // 粘包多帧时首帧可能 SetContext（重登/重连），每帧刷新 entity
    for (const auto& frame : frames)
    {
        auto* any_ptr = std::any_cast<EntityPtr>(&conn->GetContext());
        if (!any_ptr || !(*any_ptr))
        {
            LOG_WARN << "no entity mid-batch, close connection";
            conn->Shutdown();
            return;
        }
        auto entity = *any_ptr;
        auto* handler = FindHandler(frame.msg_id);
        if (handler)
        {
            auto req = CreateRequest(frame.msg_id);
            if (req)
            {
                if (!req->ParseFromString(frame.body))
                {
                    LOG_WARN << "<<< [RECV] " << msg_id_name(frame.msg_id)
                             << " bad protobuf parse, drop frame";
                    continue;  // 丢弃该帧,不关连接,继续处理后续帧
                }
            }
            handler->Handle(conn, entity, frame.msg_id, req);
        }
        else
        {
            LOG_WARN << "<<< [RECV] " << msg_id_name(frame.msg_id)
                     << " unhandled msg_id=" << frame.msg_id;
        }
    }
}

std::shared_ptr<::google::protobuf::Message> GameServer::CreateRequest(
    uint32_t msg_id) const
{
    auto it = proto_factories_.find(msg_id);
    return it != proto_factories_.end() ? it->second() : nullptr;
}

void GameServer::SendFrame(const ::zrpc::TcpConnectionPtr& conn,
                            uint32_t msg_id, const std::string& body,
                            uint8_t* seq, const std::string& proto_name)
{
    if (!conn || !conn->Connected() || !seq)
    {
        return;
    }
    PackFrame frame;
    frame.flags      = kPackFlagEncrypt;
    frame.msg_id     = msg_id;
    frame.recv_index = (*seq)++;
    frame.body       = body;
    ::zrpc::Buffer out;
    EncodeFrame(frame, &out);
    if (msg_id != proto_id("cli_heart_beat_res"))
    {
        LOG_INFO << ">>> [SEND] " << (proto_name.empty() ? msg_id_name(msg_id) : proto_name)
                 << " msg_id=" << msg_id << " body=" << body.size()
                 << "B seq=" << (uint32_t)frame.recv_index;
    }
    conn->Send(&out);
}

void GameServer::KickAndShutdownConnection(
    const EntityPtr& entity, uint32_t code_id, const char* reason,
    const ::zrpc::TcpConnectionPtr& except_conn)
{
    if (!entity)
    {
        return;
    }
    auto* old_conn_comp = entity->GetComponent<ConnectionComponent>();
    if (!old_conn_comp || !old_conn_comp->conn_ ||
        !old_conn_comp->conn_->Connected())
    {
        return;
    }
    if (except_conn && old_conn_comp->conn_ == except_conn)
    {
        return;
    }
    ::cli_kickoff_player_ntf kickoff;
    auto* old_role = entity->GetComponent<RoleComponent>();
    kickoff.set_role_id(old_role ? old_role->role_id_ : 0);
    kickoff.set_code_id(code_id);
    kickoff.set_reason(reason ? reason : "");
    SendMsg(old_conn_comp->conn_, proto_id("cli_kickoff_player_ntf"), kickoff,
            &old_conn_comp->send_seq_);
    // 被踢下线：flush 最后位置（不等 tick）。
    if (player_persist_ && old_role && old_role->role_id_ != 0)
    {
        player_persist_->FlushPlayer(old_role->role_id_);
    }
    old_conn_comp->conn_->SetContext(EntityPtr{});
    old_conn_comp->conn_->Shutdown();
    LOG_INFO << "kickoff connection entity=" << entity->GetId()
             << " code=" << code_id << " reason=" << (reason ? reason : "");
}

void GameServer::CheckHeartbeatTimeout()
{
    uint64_t now = static_cast<uint64_t>(std::time(nullptr));
    std::vector<EntityPtr> timeout_entities;

    const auto all_players = PlayerEntitySystem::Instance().GetAllByUidSnapshot();
    for (const auto& [uid, entity] : all_players)
    {
        (void)uid;
        auto* conn = entity->GetComponent<ConnectionComponent>();
        if (!conn || !conn->conn_)
        {
            continue;
        }
        if (entity->GetState() == Entity::State::kConnected ||
            entity->GetState() == Entity::State::kDisconnected)
        {
            continue;
        }
        // last_heartbeat_sec_==0：尚未收到心跳，避免刚握手误杀
        if (conn->last_heartbeat_sec_ > 0 &&
            (now - conn->last_heartbeat_sec_) >
                static_cast<uint64_t>(HeartbeatTimeoutSec()))
        {
            timeout_entities.push_back(entity);
        }
    }

    for (const auto& entity : timeout_entities)
    {
        auto* conn = entity->GetComponent<ConnectionComponent>();
        auto* role = entity->GetComponent<RoleComponent>();
        LOG_WARN << "heartbeat timeout, kicking entity=" << entity->GetId()
                 << " role_id=" << (role ? role->role_id_ : 0)
                 << " last_heartbeat=" << conn->last_heartbeat_sec_
                 << " now=" << now;

        if (entity->IsInMap())
        {
            world_->LeaveMap(entity);
        }

        entity->SetState(Entity::State::kDisconnected);

        // RemoveComponent 会销毁 ConnectionComponent，先拷贝 TcpConnectionPtr
        ::zrpc::TcpConnectionPtr conn_ptr;
        if (conn && conn->conn_)
        {
            conn_ptr = conn->conn_;
            conn_ptr->SetContext(EntityPtr{});  // 防随后 disconnect 误处理
        }

        entity->RemoveComponent<ConnectionComponent>();

        // 心跳超时与正常断线一致：立即 flush，避免脏位置丢失
        if (player_persist_)
        {
            if (role && role->role_id_ != 0)
            {
                player_persist_->FlushPlayer(role->role_id_);
            }
        }

        if (conn_ptr)
        {
            conn_ptr->Shutdown();
        }
    }

    if (!timeout_entities.empty())
    {
        LOG_INFO << "heartbeat check: kicked " << timeout_entities.size()
                 << " timeout players";
    }
}

