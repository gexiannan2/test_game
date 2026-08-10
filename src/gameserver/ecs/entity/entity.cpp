// Entity 构造、Transform 委托、脏属性通知、格心换算。

#include "ecs/entity/entity.h"

#include <cassert>

#include "ecs/components/transform_component.h"
#include "ecs/components/role_component.h"
#include "ecs/components/account_component.h"
#include "ecs/components/map_component.h"
#include "ecs/components/player_data_component.h"
#include "common/player_config.h"
#include "ecs/systems/map_system.h"
#include "ecs/systems/world_system.h"
#include "ecs/systems/aoi_system.h"

Entity::Entity(WorldSystem* world, uint64_t entity_id, EntityType type,
               const EntitySpawn& spawn)
    : world_(world),
      id_(entity_id),
      type_(type)
{
    auto& tfm = AddComponent<TransformComponent>();
    tfm.pos_ = spawn.position;
    tfm.rot_ = spawn.rotation;
    tfm.scale_ = spawn.scale;
    tfm.collision_ = spawn.collision;
}

const JPH::Vec3& Entity::GetPosition() const
{
    static const JPH::Vec3 kZero = JPH::Vec3::sZero();
    const auto* tfm = GetComponent<TransformComponent>();
    return tfm ? tfm->pos_ : kZero;
}

const JPH::Quat& Entity::GetDirection() const
{
    static const JPH::Quat kIdentity = JPH::Quat::sIdentity();
    const auto* tfm = GetComponent<TransformComponent>();
    return tfm ? tfm->rot_ : kIdentity;
}

void Entity::SetPosition(const JPH::Vec3& position, EntityPropertyType type)
{
    MutateTransform(
        [&](auto& tfm)
        {
            tfm.pos_ = position;
        },
        type);
}

void Entity::SetDirection(const JPH::Quat& rotation, EntityPropertyType type)
{
    MutateTransform(
        [&](auto& tfm)
        {
            tfm.rot_ = rotation;
        },
        type);
}

const JPH::Quat& Entity::GetMoveRot() const
{
    static const JPH::Quat kIdentity = JPH::Quat::sIdentity();
    const auto* tfm = GetComponent<TransformComponent>();
    return tfm ? tfm->move_rot_ : kIdentity;
}

void Entity::SetMoveRot(const JPH::Quat& move_rot, EntityPropertyType type)
{
    MutateTransform(
        [&](auto& tfm)
        {
            tfm.move_rot_ = move_rot;
        },
        type);
}

JPH::Vec3 Entity::GetVelocity() const
{
    auto* tfm = GetComponent<TransformComponent>();
    return tfm ? tfm->velocity_ : JPH::Vec3::sZero();
}

void Entity::SetVelocity(const JPH::Vec3& vel, EntityPropertyType type)
{
    MutateTransform(
        [&](auto& tfm)
        {
            tfm.velocity_ = vel;
        },
        type);
}

void Entity::SetMoveState(const JPH::Quat& rot,
                          const JPH::Quat& move_rot,
                          const JPH::Vec3& vel,
                          EntityPropertyType type)
{
    MutateTransform(
        [&](auto& tfm)
        {
            tfm.rot_ = rot;
            tfm.move_rot_ = move_rot;
            tfm.velocity_ = vel;
        },
        type);
}

float Entity::GetHeight() const
{
    const auto* tfm = GetComponent<TransformComponent>();
    return tfm ? tfm->height_ : 0.0f;
}

float Entity::GetRadius() const
{
    const auto* tfm = GetComponent<TransformComponent>();
    return tfm ? tfm->radius_ : 0.0f;
}

void Entity::SetHeight(float h)
{
    auto* tfm = GetComponent<TransformComponent>();
    if (tfm)
    {
        tfm->height_ = h;
    }
}

void Entity::SetRadius(float r)
{
    auto* tfm = GetComponent<TransformComponent>();
    if (tfm)
    {
        tfm->radius_ = r;
    }
}

float Entity::GetCapsuleHalfHeight() const
{
    float half = (GetHeight() - 2.0f * GetRadius()) * 0.5f;
    return half > 0.01f ? half : 0.01f;
}

JPH::Vec3 Entity::GetBodyCenter() const
{
    JPH::Vec3 pos = GetPosition();
    pos.SetY(pos.GetY() + GetHeight() * 0.5f);
    return pos;
}

void Entity::InitCapsuleParams()
{
    auto& cfg = PlayerConfig::Instance();
    SetRadius(cfg.capsule_radius);
    SetHeight(cfg.capsule_height);
}

void Entity::TransferGameplayComponentsFrom(const EntityPtr& from)
{
    if (!from || from.get() == this)
    {
        return;
    }

    if (auto* src = from->GetComponent<RoleComponent>())
    {
        if (!HasComponent<RoleComponent>())
        {
            AddComponent<RoleComponent>();
        }
        *GetComponent<RoleComponent>() = *src;
    }

    if (auto* src = from->GetComponent<TransformComponent>())
    {
        if (!HasComponent<TransformComponent>())
        {
            AddComponent<TransformComponent>();
        }
        *GetComponent<TransformComponent>() = *src;
    }

    if (auto* src = from->GetComponent<MapComponent>())
    {
        if (!HasComponent<MapComponent>())
        {
            AddComponent<MapComponent>();
        }
        *GetComponent<MapComponent>() = *src;
    }

    if (auto* src = from->GetComponent<PlayerDataComponent>())
    {
        if (!HasComponent<PlayerDataComponent>())
        {
            AddComponent<PlayerDataComponent>();
        }
        *GetComponent<PlayerDataComponent>() = *src;
    }
}

std::string Entity::LogTag() const
{
    std::string s;
    s.reserve(64);
    uint64_t role_id = 0;
    const char* name = "";
    auto* role = GetComponent<RoleComponent>();
    if (role)
    {
        role_id = role->role_id_;
        name = role->name_.c_str();
    }
    const char* uid = "";
    auto* acc = GetComponent<AccountComponent>();
    if (acc)
    {
        uid = acc->uid_.c_str();
    }
    s += "role_id=";
    s += std::to_string(role_id);
    s += " uid=";
    s += (uid[0] ? uid : "-");
    s += " name=";
    s += (name[0] ? name : "-");
    return s;
}

int32_t Entity::GetScale() const
{
    const auto* tfm = GetComponent<TransformComponent>();
    return tfm ? tfm->scale_ : 0;
}

int32_t Entity::GetCollision() const
{
    const auto* tfm = GetComponent<TransformComponent>();
    return tfm ? tfm->collision_ : 0;
}

void Entity::AppendPropertyDirty(EntityPropertyType typ)
{
    for (EntityPropertyType existing : dirty_property_types_)
    {
        if (existing == typ)
        {
            return;
        }
    }
    dirty_property_types_.push_back(typ);
}

void Entity::ClearPropertyTypes()
{
    dirty_property_types_.clear();
}

void Entity::SetPropertyDirty(EntityPropertyType typ, bool sync_immediately)
{
    if (world_ == nullptr)
    {
        return;
    }
    AppendPropertyDirty(typ);
    world_->Aoi().MarkPropertyDirty(SharedHandle(), sync_immediately);
}

void Entity::SetPropertyDirty(std::initializer_list<EntityPropertyType> types, bool sync_immediately)
{
    if (world_ == nullptr)
    {
        return;
    }
    for (EntityPropertyType typ : types)
    {
        AppendPropertyDirty(typ);
    }
    world_->Aoi().MarkPropertyDirty(SharedHandle(), sync_immediately);
}

Vector3D Entity::GetGridCenter() const
{
    Vector3D pos = GetPosition();
    // 胶囊体（kPlayer）使用几何中心：脚底 + 半高；其他类型脚底即中心
    if (type_ == EntityType::kPlayer)
    {
        pos.SetY(pos.GetY() + GetHeight() * 0.5f);
    }
    if (world_ != nullptr)
    {
        world_->Map().MapIndexToCenterPos(pos.GridX(), pos.GridY(), pos.GridZ(), pos);
    }
    return pos;
}
