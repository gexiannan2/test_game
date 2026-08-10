// 场景总线：进离图、移动、实体注册表。
//
// map_  = 地图足迹（MapGrid, 物理位置）
// aoi_  = 视野通知（AoiSector, 逻辑分区）
//
// 调用原则：
//   WorldSystem 是唯一协调者，map_ 和 aoi_ 不互调。
//   map_ 只做空间索引，aoi_ 只做视野广播，entity 是数据载体。

#include "ecs/systems/world_system.h"

#include <vector>

#include "common/event_bus.h"
#include "jolt_server.h"
#include "zrpc/base/logger.h"

WorldSystem::WorldSystem(SceneRegionType system_type)
    : map_(std::make_unique<MapSystem>(system_type))
{
}

std::shared_ptr<WorldSystem> WorldSystem::Create(SceneRegionType system_type)
{
    return std::shared_ptr<WorldSystem>(new WorldSystem(system_type));
}

void WorldSystem::Init()
{
    if (initialized_ || !map_)
    {
        return;
    }

    map_->Init();
    aoi_.BindMapWorld(map_.get());
    aoi_.BindWorld(this);
    monster_ai_system_.Bind(this);
    full_move_system_.Bind(this, map_.get());
    initialized_ = true;
}

void WorldSystem::Tick(float dt)
{
    // 统一 30Hz 步进，避免多 timer 竞态
    // 1. 怪物行为树低频决策（下发 MoveSystem 指令）
    monster_ai_system_.Tick(dt);

    // 2. NPC 移动逻辑（写 TransformComponent）
    full_move_system_.Tick(dt);

    // 3. Jolt 物理：同步所有 body 到 TransformComponent 最新位置
    jolt_system_.SyncAllBodies(dt);

    // 4. Jolt 物理步进
    if (jolt_server_)
    {
        jolt_server_->Update(dt, 1);
    }

    // 5. AOI 脏数据广播
    aoi_.FlushDirty();
}

void WorldSystem::SetEntityFactory(EntityFactory factory)
{
    factory_ = std::move(factory);
}

EntityPtr WorldSystem::Spawn(EntityType type, const EntitySpawn& spawn)
{
    if (factory_)
    {
        return factory_(this, next_entity_id_++, type, spawn);
    }

    return nullptr;
}

EntityPtr WorldSystem::SpawnOnMap(EntityType type, const EntitySpawn& spawn)
{
    EntityPtr entity = Spawn(type, spawn);

    if (entity)
    {
        EnterMap(entity);
    }

    return entity;
}

// ============================================================
// EnterMap — 三步：地图足迹 → entity 状态 → AOI 注册
// ============================================================

void WorldSystem::EnterMap(const EntityPtr& entity)
{
    if (!entity || !map_ || !initialized_ || entity->IsInMap())
    {
        return;
    }

    RegisterEntity(entity);

    // 1. 地图足迹
    map_->OnEntityIntoMap(entity);

    // 2. entity 状态
    entity->SetInMap(true);
    entity->SetWorld(this);

    // 3. AOI: 广播自身出现 + 注册视野
    aoi_.OnEntityIntoMap(entity);

    if (entity->NeedsAoiWatcher())
    {
        aoi_.AddWatcher(entity, entity->GetGridCenter());
        // 显式补发自身 appear（is_self=true）：仅进图一次。
        // NotifyAppearToReceiver 已跳过自身，移动跨格 AddWatcher 不会再重复推。
        aoi_.NotifySelfAppear(entity);
    }

    // 4. Jolt 物理：创建运动学胶囊体
    jolt_system_.OnEntityEnterMap(entity);

    // 5. 广播进图事件（AoiViewBridge 发 enter_map_ntf；jump/统计等业务模块监听）
    //    进/离/更新地图统一走 EventBus，MapSystem 不再持有业务回调。
    EventBus::Instance().Publish(EvtEnterMap{entity});
}

// ============================================================
// LeaveMap — 逆序：AOI 注销 → entity 状态 → 地图足迹清理
// ============================================================

void WorldSystem::LeaveMap(const EntityPtr& entity)
{
    if (!entity || !map_ || !initialized_ || !entity->IsInMap())
    {
        return;
    }

    // 1. 先停止并销毁行为树，防止离图后继续下发动作
    monster_ai_system_.DetachMonster(entity->GetId());

    // 2. 移动系统收尾
    full_move_system_.CancelMove(entity, MoveStopReason::kStopCommand);

    // 2.1 清零 TransformComponent.velocity：
    //     CancelMove 只清 MoveComponent 内部路径状态，不写 TransformComponent。
    //     如果玩家在移动中断线，velocity 会保留非零值，SyncToData 落地后
    //     下次上线 SyncFromData 恢复非零 velocity → 玩家自动漂移。
    //     用 kNone 不触发 OnTransformChanged（LeaveMap 不应产生额外脏标记）。
    entity->SetVelocity(JPH::Vec3::sZero(), EntityPropertyType::kNone);

    // 3. AOI: 广播自身消失 → 移除视野
    aoi_.OnEntityLeaveMap(entity);

    if (aoi_.HasWatcher(entity->GetId()))
    {
        aoi_.RemoveWatcher(entity->GetId(), entity->GetPosition(), false);
    }

    // 4. Jolt 物理：删除胶囊体
    jolt_system_.OnEntityLeaveMap(entity);

    // 5. entity 状态
    entity->SetInMap(false);
    entity->SetWorld(nullptr);
    entity->ClearPropertyTypes();

    // 6. 地图足迹清理
    map_->OnEntityLeaveMap(entity);
    UnregisterEntity(entity->GetId());

    // 7. 广播离图事件（jump 清冷却等业务模块监听）
    EventBus::Instance().Publish(EvtLeaveMap{entity});
}

// ============================================================
// MoveEntity — 仅跨格时操作 map 足迹；AOI 内部已有同格检查
// ============================================================

void WorldSystem::MoveEntity(const EntityPtr& entity, const Vector3D& new_pos,
                             EntityPropertyType type)
{
    if (!entity || !map_ || !initialized_)
    {
        return;
    }

    if (!entity->IsInMap())
    {
        entity->SetPosition(new_pos, type);
        return;
    }

    Vector3D clamped = new_pos;

    if (bounds_clamp_)
    {
        clamped = bounds_clamp_(new_pos);
    }

    Vector3D old_pos      = entity->GetPosition();
    Vector3D old_center   = entity->GetGridCenter();
    entity->SetPosition(clamped, type);
    const Vector3D& actual_pos = entity->GetPosition();
    Vector3D new_center   = entity->GetGridCenter();

    bool grid_changed = (old_pos.GridX() != actual_pos.GridX() ||
                         old_pos.GridY() != actual_pos.GridY() ||
                         old_pos.GridZ() != actual_pos.GridZ());

    // 1. 地图足迹：仅跨格时更新
    if (grid_changed)
    {
        map_->OnEntityChangePos(entity, old_pos, actual_pos);
    }

    // 2. AOI subject 移动广播（跨格生成 appear/disappear）
    aoi_.OnEntityChangePos(entity, old_pos, actual_pos);

    // 3. AOI watcher 邻域重建（内部同格 return true）
    if (entity->NeedsAoiWatcher() && aoi_.HasWatcher(entity->GetId()))
    {
        if (!aoi_.MoveWatcher(entity, old_center, new_center))
        {
            LOG_WARN << "MoveWatcher failed entity=" << entity->GetId()
                     << " — self-heal Remove+Add watcher";
            aoi_.RemoveWatcher(entity, false);

            if (!aoi_.AddWatcher(entity, new_center))
            {
                // Add 失败时回退到旧中心，避免 watcher 丢失
                LOG_WARN << "AddWatcher(new) failed entity=" << entity->GetId()
                         << " — fallback AddWatcher(old_center)";
                aoi_.AddWatcher(entity, old_center);
            }
        }
    }

    // 4. 跨格时发布"更新地图"事件（业务模块监听 EvtMoveMap）
    if (grid_changed)
    {
        EventBus::Instance().Publish(EvtMoveMap{entity, old_pos, actual_pos});
    }
}

// ============================================================
// 辅助方法
// ============================================================

void WorldSystem::UpdateEntity(const EntityPtr& entity)
{
    if (!entity || !initialized_ || !entity->IsInMap())
    {
        return;
    }

    aoi_.MarkPropertyDirty(entity);
}

std::vector<uint64_t> WorldSystem::GetVisibleEntities(uint64_t watcher_id) const
{
    return aoi_.GetVisibleEntities(watcher_id);
}

bool WorldSystem::IsWatcher(uint64_t entity_id) const
{
    return aoi_.HasWatcher(entity_id);
}

void WorldSystem::RegisterEntity(const EntityPtr& entity)
{
    if (!entity)
    {
        return;
    }

    std::lock_guard<std::mutex> lk(entity_mutex_);
    entity_index_[entity->GetId()] = entity;
}

void WorldSystem::UnregisterEntity(uint64_t entity_id)
{
    std::lock_guard<std::mutex> lk(entity_mutex_);
    entity_index_.erase(entity_id);
}

EntityPtr WorldSystem::FindEntity(uint64_t entity_id) const
{
    std::lock_guard<std::mutex> lk(entity_mutex_);
    auto it = entity_index_.find(entity_id);

    if (it == entity_index_.end())
    {
        return nullptr;
    }

    return it->second.lock();
}

size_t WorldSystem::GetEntityCount() const
{
    std::lock_guard<std::mutex> lk(entity_mutex_);
    return entity_index_.size();
}
