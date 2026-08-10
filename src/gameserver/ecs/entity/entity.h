// 场景实体基类：组件槽 + AOI 序列化。位置/朝向在 TransformComponent，不在此类重复存储。

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <Jolt/Jolt.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Quat.h>

#include "ecs/component_base/component_base.h"
#include "ecs/components/move_component.h"
#include "ecs/components/transform_component.h"
#include "common/aoi_def.h"
#include "client_common.pb.h"  // entity_player_data

namespace google
{
    namespace protobuf
    {
        class Message;
    }  // namespace protobuf
}  // namespace google

// 一次 AOI 同步产物：协议号 + protobuf 消息体。
struct SerializeMsg
{
    uint32_t msg_id = 0;
    std::shared_ptr<::google::protobuf::Message> msg;
};

// 可脏同步的属性类型。变化时 SetPropertyDirty → Flush → SerializeDirty。
enum class EntityPropertyType : uint32_t
{
    kNone = 0,
    kStopMove = 1,     // pos/rot/velocity（velocity 应已清零）→ cli_3d_aoi_update_ntf
    kMove = 2,          // pos/rot/velocity                     → cli_3d_aoi_update_ntf
    kHomeObject = 3,    // 玩家实体通常忽略
    kUseEmoji = 4,      // 表现类                                      → 跳过
    kAttrUpdate = 5,    // HP/MP/属性变化                        → cli_3d_aoi_attr_update_ntf
    kJump = 6,          // 跳跃      → cli_3d_jump_req (aoi_update_ntf: entity_jump_data)
    kDodge = 7,         // 闪避      → cli_3d_dodge_req (aoi_update_ntf: entity_dodge_data)
};

class WorldSystem;
class MoveComponent;

struct EntitySpawn
{
    JPH::Vec3 position = JPH::Vec3::sZero();
    JPH::Quat rotation = JPH::Quat::sIdentity();
    int32_t scale     = 0;
    int32_t collision = 0;

    static EntitySpawn At(const JPH::Vec3& position)
    {
        EntitySpawn spawn;
        spawn.position = position;
        return spawn;
    }
};

// 场景逻辑物体基类；须 shared_ptr 管理。构造时自动挂载 TransformComponent。
class Entity : public std::enable_shared_from_this<Entity>
{
    public:
        Entity(WorldSystem* world, uint64_t entity_id, EntityType type,
               const EntitySpawn& spawn);
        virtual ~Entity() = default;

        std::shared_ptr<Entity> SharedHandle()
        {
            return shared_from_this();
        }

        uint64_t GetId() const
        {
            return id_;
        }

        uint64_t GetEntityId() const
        {
            return id_;
        }

        EntityType GetEntityType() const
        {
            return type_;
        }

        int32_t GetScale() const;
        int32_t GetCollision() const;

        const JPH::Vec3& GetPosition() const;
        const JPH::Quat& GetDirection() const;
        const JPH::Quat& GetMoveRot() const;
        JPH::Vec3 GetVelocity() const;

        // type 默认 kNone（无效）：仅写 Transform 不自动标脏，由调用方自行
        // SetPropertyDirty 决定同步类型。需要自动标脏时显式传 kMove/kJump/kDodge 等。
        void SetPosition(const JPH::Vec3& position,
                         EntityPropertyType type = EntityPropertyType::kNone);
        void SetDirection(const JPH::Quat& rotation,
                          EntityPropertyType type = EntityPropertyType::kNone);
        void SetMoveRot(const JPH::Quat& move_rot,
                        EntityPropertyType type = EntityPropertyType::kNone);
        void SetVelocity(const JPH::Vec3& vel,
                         EntityPropertyType type = EntityPropertyType::kNone);
        void SetMoveState(const JPH::Quat& rot,
                          const JPH::Quat& move_rot,
                          const JPH::Vec3& vel,
                          EntityPropertyType type = EntityPropertyType::kNone);

        float GetHeight() const;
        float GetRadius() const;
        void SetHeight(float h);
        void SetRadius(float r);

        // 胶囊圆柱段半高 = (height - 2*radius) * 0.5
        float GetCapsuleHalfHeight() const;
        JPH::Vec3 GetBodyCenter() const;

        // 用 PlayerConfig 默认值补全未设置的 radius/height
        void InitCapsuleParams();

        // 顶号/重连时把玩法组件迁到新实体（不迁 ConnectionComponent）
        void TransferGameplayComponentsFrom(const EntityPtr& from);

        // 统一玩家标识：role_id=X uid=Y name=Z
        std::string LogTag() const;

        bool IsInMap() const
        {
            return is_in_map_.load(std::memory_order_acquire);
        }

        void SetInMap(bool in_map)
        {
            is_in_map_.store(in_map, std::memory_order_release);
        }

        // 连接/会话状态机：kConnected → kHandshaked → kLoggedIn → kRoleSelected
        //   → kDataLoading（异步加载 DB 存档）→ kInGame；
        // 任意阶段可 → kDisconnected。由 Connection/Account/RoleSystem、Handler 推进。
        // kDataLoading：cli_enter_game_req 首次进图异步加载存档期间置位；
        //   各 handler 按 state 校验拒绝业务消息（move 要求 kInGame、role 要求 kLoggedIn）；
        //   加载回调据此校验丢弃顶号/重连后的过期结果（防 UAF 与身份错乱）。
        enum class State : int
        {
            kConnected = 0,
            kHandshaked,
            kLoggedIn,
            kRoleSelected,
            kDataLoading,
            kInGame,
            kDisconnected,
        };

        State GetState() const
        {
            return state_;
        }

        void SetState(State s)
        {
            state_ = s;
        }

        template <typename T, typename... Args>
        T& AddComponent(Args&&... args)
        {
            return components_.Emplace<T>(std::forward<Args>(args)...);
        }

        template <typename T>
        T* GetComponent()
        {
            return components_.Get<T>();
        }

        template <typename T>
        const T* GetComponent() const
        {
            return components_.Get<T>();
        }

        template <typename T>
        bool HasComponent() const
        {
            return components_.Has<T>();
        }

        template <typename T>
        bool RemoveComponent()
        {
            return components_.Remove<T>();
        }

        MoveComponent* Move()
        {
            return GetComponent<MoveComponent>();
        }

        const MoveComponent* Move() const
        {
            return GetComponent<MoveComponent>();
        }

        // 脏属性：SetPropertyDirty → AoiSystem 脏队列 → Flush → SerializeDirty。
        // 同一 EntityPropertyType 在 dirty_property_types_ 中只保留一条。
        bool IsDirty() const
        {
            return !dirty_property_types_.empty();
        }

        const std::vector<EntityPropertyType>& PropertyTypes() const
        {
            return dirty_property_types_;
        }

        void ClearPropertyTypes();
        void SetPropertyDirty(EntityPropertyType typ, bool sync_immediately = false);
        void SetPropertyDirty(std::initializer_list<EntityPropertyType> types, bool sync_immediately = false);

        virtual bool IsPlayer() const
        {
            return false;
        }

        virtual bool IsPlayerEntity() const
        {
            return IsPlayer();
        }

        virtual bool NeedsAoiWatcher() const
        {
            return IsPlayer();
        }

        // Transform 写入钩子：SetPosition/SetMoveState 等在写入成功后回调。
        // PlayerEntity override 后自动标脏 PlayerDataComponent，业务点无需手动 MarkPersistDirty。
        // 加载流程用 SuppressPersistDirty 屏蔽。基类空实现（非玩家实体无需持久化）。
        virtual void OnTransformChanged(EntityPropertyType type = EntityPropertyType::kMove)
        {
        }

        template<typename Fn>
        void MutateTransform(Fn&& fn, EntityPropertyType type)
        {
            auto* tfm = GetComponent<TransformComponent>();
            if (!tfm)
            {
                return;
            }

            fn(*tfm);

            // kNone：仅写 Transform，不自动标脏；由调用方自行 SetPropertyDirty 决定同步类型。
            if (type != EntityPropertyType::kNone)
            {
                OnTransformChanged(type);
            }
        }

        // SerializeAppear = 全量（进视野）；SerializeDirty = 增量（仅脏字段）。
        virtual bool SerializeAppear(SerializeMsg& out,
                                     const EntityPtr& observer) = 0;
        virtual bool SerializeDirty(std::vector<SerializeMsg>& out_msgs) = 0;

        // 组装玩家 DB 快照（值语义）；非玩家实体默认返回 nullopt。
        // sequence 由调用方（PlayerPersistSystem）维护并传入，用于乱序覆盖可观测。
        // 返回协议原生 entity_player_data（空间+基础属性+战斗），由 mongo 存储层序列化落库。
        virtual std::optional<::entity_player_data> SerializeToDB(uint64_t sequence) const
        {
            (void)sequence;
            return std::nullopt;
        }

        Vector3D GetGridCenter() const;

        void SetWorld(WorldSystem* world)
        {
            world_ = world;
        }

        WorldSystem* GetWorld() const
        {
            return world_;
        }

    private:
        void AppendPropertyDirty(EntityPropertyType typ);

        WorldSystem* world_ = nullptr;
        uint64_t id_;
        EntityType type_;
        std::atomic<bool> is_in_map_{false};
        State state_ = State::kConnected;
        ComponentStorage components_;
        std::vector<EntityPropertyType> dirty_property_types_;
};

using EntityFactory = std::function<EntityPtr(
    WorldSystem* world, uint64_t entity_id, EntityType type,
    const EntitySpawn&)>;
