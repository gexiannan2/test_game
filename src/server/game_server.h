#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>

#include "ecs/entity/entity.h"
#include "ecs/systems/aoi_view_bridge.h"
#include "jolt_server.h"
#include "session/system.h"
#include "ecs/systems/world_system.h"
#include "handlers/handler_base.h"
#include "protocol/pack_codec.h"
#include "zrpc/base/logger.h"
#include "zrpc/net/event_loop.h"
#include "zrpc/net/tcp_server.h"

#include "ecs/systems/player_persist_system.h"

namespace mongo
{
    class PlayerMongoStorage;
} // namespace mongo

// 游戏服编排：TCP 收发包、Handler 分发、WorldSystem/AoiViewBridge 生命周期
// 继承 enable_shared_from_this：所有跨线程 lambda 通过 weak_ptr 捕获，
class GameServer : public std::enable_shared_from_this<GameServer>
{
    public:
        static constexpr int kHeartbeatCheckIntervalSec = 5;
        static constexpr int kHeartbeatTimeoutSec       = 30;  // 无心跳踢线
        static constexpr double kWorldTickIntervalSec   = 1.0 / 30.0;  // Move + Jolt + AOI 统一 30Hz

        GameServer(const std::string& ip, int port);
        ~GameServer();

        void Start();  // 监听失败时置 listen_failed_，调用方应检查 ListenOk()
        bool ListenOk() const { return !listen_failed_; }
        void Loop();
        void Stop();  // 须在 loop 线程调用

        void RunInLoop(std::function<void()> fn);

        // 测试钩子：立即跑一轮心跳超时扫描
        void CheckHeartbeatTimeoutForTest()
        {
            CheckHeartbeatTimeout();
        }

        void RegisterHandler(uint32_t msg_id, std::unique_ptr<IHandler> handler);
        IHandler* FindHandler(uint32_t msg_id) const;

        // 注册 handler + proto 反序列化工厂（框架在 OnMessage 统一 ParseFromString）。
        // ReqMsg=void 表示该消息无请求体（如心跳），req 传 nullptr 给 Handler。
        template <typename ReqMsg>
        void RegisterTypedHandler(uint32_t msg_id, IHandler* handler)
        {
            handler->SetServer(this);
            handlers_[msg_id] = handler;
            if constexpr (!std::is_same_v<ReqMsg, void>)
            {
                proto_factories_[msg_id] = []() -> std::shared_ptr<::google::protobuf::Message> {
                    return std::make_shared<ReqMsg>();
                };
            }
        }

        void SendFrame(const ::zrpc::TcpConnectionPtr& conn,
                       uint32_t msg_id, const std::string& body,
                       uint8_t* seq, const std::string& proto_name = "");

        template <typename Msg>
        void SendMsg(const ::zrpc::TcpConnectionPtr& conn,
                     uint32_t msg_id, const Msg& msg, uint8_t* seq)
        {
            std::string body;
            if (!msg.SerializeToString(&body))
            {
                LOG_WARN << "SendMsg SerializeToString failed, msg_id=" << msg_id;
                return;
            }
            SendFrame(conn, msg_id, body, seq);
        }

        // 重载: 类型擦除的 Message（供桥接层/AOI ntf 使用，与 SendMsg 走同一路径）
        void SendMsg(const ::zrpc::TcpConnectionPtr& conn,
                     uint32_t msg_id,
                     const std::shared_ptr<::google::protobuf::Message>& msg,
                     uint8_t* seq)
        {
            if (!msg) return;
            std::string body;
            if (!msg->SerializeToString(&body))
            {
                LOG_WARN << "SendMsg(Message) SerializeToString failed, msg_id=" << msg_id;
                return;
            }
            SendFrame(conn, msg_id, body, seq);
        }

        // 重载: 已序列化的 body（供桥接层 is_self 克隆后使用）
        void SendMsg(const ::zrpc::TcpConnectionPtr& conn,
                     uint32_t msg_id, const std::string& body, uint8_t* seq)
        {
            SendFrame(conn, msg_id, body, seq);
        }

        WorldSystem* GetWorld()
        {
            return world_.get();
        }
        AoiViewBridge* GetAoiBridge()
        {
            return aoi_bridge_.get();
        }
        JoltServer* GetJoltServer()
        {
            return jolt_server_.get();
        }

        PlayerPersistSystem* GetPlayerPersist()
        {
            return player_persist_.get();
        }

        mongo::PlayerMongoStorage* GetPlayerStorage()
        {
            return player_storage_.get();
        }

        // 查询玩家存档（QueryRole）：异步从 mongo 加载，回调在业务线程。
        // success=true 表示查到存档；data 为存档（无存档时默认构造）。
        void QueryRole(uint64_t roleId, std::function<void(bool, ::entity_player_data)> cb);

        // 删除玩家存档：异步从 mongo 删除（角色删除时调用）。
        // cb 在业务线程执行；默认空（删除结果通常不关心）。
        void DeleteRoleArchive(uint64_t roleId, std::function<void(bool)> cb = {});

        uint64_t GenSessionId()
        {
            return next_session_id_.fetch_add(1);  // 起点 10001
        }
        uint64_t GenRoleId()
        {
            return next_role_id_.fetch_add(1);  // 起点 20001
        }

        std::string GateAddr() const { return ip_ + ":" + std::to_string(port_); }

        // 踢旧 TCP：发 kickoff → 清 Context → Shutdown（不碰 send_seq_ 所属实体）
        void KickAndShutdownConnection(const EntityPtr& entity, uint32_t code_id,
                                       const char* reason,
                                       const ::zrpc::TcpConnectionPtr& except_conn =
                                           nullptr);

    private:
        void OnConnection(const ::zrpc::TcpConnectionPtr& conn);
        void OnMessage(const ::zrpc::TcpConnectionPtr& conn, ::zrpc::Buffer* buf);

        // msg_id → 空 protobuf 对象工厂（框架统一反序列化用）
        std::shared_ptr<::google::protobuf::Message> CreateRequest(uint32_t msg_id) const;

        void LoadConfigs();
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
        std::unordered_map<uint32_t, IHandler*> handlers_;
        std::unordered_set<std::unique_ptr<IHandler>> handler_owners_;
        std::unordered_map<uint32_t,
                           std::function<std::shared_ptr<::google::protobuf::Message>()>>
            proto_factories_;
        std::atomic<uint64_t> next_session_id_{10001};
        std::atomic<uint64_t> next_role_id_{20001};
        std::atomic<bool> stopping_{false};  // 防重复触发 drain

        std::shared_ptr<WorldSystem> world_;
        std::unique_ptr<AoiViewBridge> aoi_bridge_;
        std::unique_ptr<JoltServer> jolt_server_;

        std::unique_ptr<mongo::PlayerMongoStorage> player_storage_;
        std::unique_ptr<PlayerPersistSystem> player_persist_;

        std::shared_ptr<::zrpc::Timer> heartbeat_timer_;
        std::shared_ptr<::zrpc::Timer> world_tick_timer_;
        std::shared_ptr<::zrpc::Timer> persist_timer_;
        std::shared_ptr<::zrpc::Timer> mongo_ping_timer_;
};
