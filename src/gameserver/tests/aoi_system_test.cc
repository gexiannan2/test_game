// AOI/Map 系统单机测试：进离移动、跨格视野、回调计数（不走网络）。

#include "test_harness.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "ecs/entity/entity.h"
#include "ecs/components/transform_component.h"
#include "common/aoi_def.h"
#include "ecs/systems/world_system.h"

namespace {

// 测试用简单实体：覆写 IsPlayer 让玩家成为观察者；
// SerializeAppear/SerializeDirty 给空实现（测试只数回调次数，不发真实包）
class TestEntity : public Entity {
 public:
    TestEntity(WorldSystem* world, uint64_t id, EntityType type,
               const EntitySpawn& spawn)
        : Entity(world, id, type, spawn) {}

    bool IsPlayer() const override { return GetEntityType() == EntityType::kPlayer; }

    bool SerializeAppear(SerializeMsg&, const EntityPtr&) override {
        return false;
    }
    bool SerializeDirty(std::vector<SerializeMsg>&) override {
        return false;
    }
};

// 视野事件计数器（替代网络发包）
struct ViewCounters {
    int appear = 0;
    int disappear = 0;
    int update = 0;
    std::vector<uint64_t> appeared_ids;   // viewer 收到的 subject ids
    std::vector<uint64_t> disappeared_ids;
};

// 构造世界 + 注入视野回调（计数）
struct TestWorld {
    std::shared_ptr<WorldSystem> world;
    std::unordered_map<uint64_t, ViewCounters> counters;  // viewer_id → counters

    static std::shared_ptr<TestWorld> Create() {
        auto tw = std::make_shared<TestWorld>();
        tw->world = WorldSystem::Create(SceneRegionType::kMap);
        tw->world->Init();
        // 注入实体工厂：创建 TestEntity（让 IsPlayer() 对 kPlayer 类型返回 true）
        tw->world->SetEntityFactory(
            [](WorldSystem* w, uint64_t id, EntityType type,
               const EntitySpawn& spawn) -> EntityPtr {
                return std::make_shared<TestEntity>(w, id, type, spawn);
            });
        // 视野回调：按 viewer_id 计数
        tw->world->Aoi().SetEntityEnterCallback(
            [tw](uint64_t viewer_id, const std::vector<uint64_t>& ids) {
                auto& c = tw->counters[viewer_id];
                c.appear += static_cast<int>(ids.size());
                for (uint64_t id : ids) c.appeared_ids.push_back(id);
            });
        tw->world->Aoi().SetEntityLeaveCallback(
            [tw](uint64_t viewer_id, const std::vector<uint64_t>& ids) {
                auto& c = tw->counters[viewer_id];
                c.disappear += static_cast<int>(ids.size());
                for (uint64_t id : ids) c.disappeared_ids.push_back(id);
            });
        tw->world->Aoi().SetEntityUpdateCallback(
            [tw](uint64_t viewer_id, uint64_t subject_id) {
                auto& c = tw->counters[viewer_id];
                ++c.update;
            });
        return tw;
    }

    EntityPtr SpawnPlayer(uint64_t id, float x, float z) {
        EntitySpawn spawn;
        spawn.position = JPH::Vec3(x, 0.0f, z);
        return world->SpawnOnMap(EntityType::kPlayer, spawn);
    }
};

}  // namespace

// ---- 进图 ----
GAME_TEST_SUITE(AoiMapTest)
GAME_TEST(AoiMapTest, SinglePlayerSelfAppear) {
    auto tw = TestWorld::Create();
    auto a = tw->SpawnPlayer(1, 0.0f, 0.0f);
    EXPECT_TRUE(a->IsInMap());
    // A 自己进图，收到自身的 appear（is_self=true）
    EXPECT_EQ(tw->counters[1].appear, 1);
}

GAME_TEST(AoiMapTest, TwoPlayersSeeEachOther) {
    auto tw = TestWorld::Create();
    auto a = tw->SpawnPlayer(1, 0.0f, 0.0f);   // A 先进
    auto b = tw->SpawnPlayer(2, 0.0f, 0.0f);   // B 紧邻 A 进
    // B 进图后，A 应看到 B（appear 1）；B 也应看到 A
    EXPECT_TRUE(tw->counters[1].appear >= 1);  // A 看到 B
    EXPECT_TRUE(tw->counters[2].appear >= 1);  // B 看到 A
}

GAME_TEST(AoiMapTest, LeaveMapSendsDisappear) {
    auto tw = TestWorld::Create();
    auto a = tw->SpawnPlayer(1, 0.0f, 0.0f);
    auto b = tw->SpawnPlayer(2, 0.0f, 0.0f);
    tw->counters.clear();  // 清进图计数
    tw->world->LeaveMap(a);
    // B 应收到 A 的 disappear
    EXPECT_TRUE(tw->counters[2].disappear >= 1);
}

GAME_TEST(AoiMapTest, MoveAcrossAoiCell) {
    auto tw = TestWorld::Create();
    // A 在格 (0,0)，B 远处（超出邻域半径 kAoiRadius=1，cell_size=8）
    auto a = tw->SpawnPlayer(1, 0.0f, 0.0f);
    auto b = tw->SpawnPlayer(2, 1000.0f, 0.0f);  // 1000/8=125 格远，远超邻域
    tw->counters.clear();
    // B 移动到 A 附近（跨 AOI 格 → 应触发 appear）
    tw->world->MoveEntity(b, JPH::Vec3(1.0f, 0.0f, 1.0f), EntityPropertyType::kMove);
    EXPECT_TRUE(tw->counters[1].appear >= 1);  // A 看到 B 出现
    EXPECT_TRUE(tw->counters[2].appear >= 1);  // B 看到 A
}

GAME_TEST(AoiMapTest, MoveAwaySendsDisappear) {
    auto tw = TestWorld::Create();
    auto a = tw->SpawnPlayer(1, 0.0f, 0.0f);
    auto b = tw->SpawnPlayer(2, 1.0f, 0.0f);  // 邻近（AOI格0）
    tw->counters.clear();
    // B 远离 A：移到 AOI格125（1000/8=125），远超邻域半径1 → 应触发 disappear
    tw->world->MoveEntity(b, JPH::Vec3(1000.0f, 0.0f, 0.0f), EntityPropertyType::kMove);
    EXPECT_TRUE(tw->counters[1].disappear >= 1);  // A 看到 B 消失
}

GAME_TEST(AoiMapTest, NoBoundaryUnboundedMap) {
    // 无界地图：实体在任意坐标都能进图（不再有固定边界）
    auto tw = TestWorld::Create();
    auto far_player = tw->SpawnPlayer(1, 99999.0f, 99999.0f);
    EXPECT_TRUE(far_player->IsInMap());
    EXPECT_EQ(tw->world->GetEntityCount(), static_cast<size_t>(1));
}

GAME_TEST(AoiMapTest, UpdateEntityPushesDirty) {
    auto tw = TestWorld::Create();
    auto a = tw->SpawnPlayer(1, 0.0f, 0.0f);
    auto b = tw->SpawnPlayer(2, 1.0f, 0.0f);
    tw->counters.clear();
    // 标记 A 脏属性，Tick 后 B 应收到 update
    a->SetPropertyDirty(EntityPropertyType::kMove, /*sync_immediately=*/true);
    EXPECT_TRUE(tw->counters[2].update >= 1);
}

GAME_TEST(AoiMapTest, LeaveMapRemovesWatcher) {
    auto tw = TestWorld::Create();
    auto a = tw->SpawnPlayer(1, 0.0f, 0.0f);
    EXPECT_TRUE(tw->world->IsWatcher(1));
    tw->world->LeaveMap(a);
    EXPECT_FALSE(tw->world->IsWatcher(1));
}

GAME_TEST(AoiMapTest, GetVisibleEntities) {
    auto tw = TestWorld::Create();
    auto a = tw->SpawnPlayer(1, 0.0f, 0.0f);
    auto b = tw->SpawnPlayer(2, 0.0f, 0.0f);  // 与 A 同格
    auto c = tw->SpawnPlayer(3, 1000.0f, 0.0f);  // AOI格125，远处不可见
    // A 看到 B（进图时 appear 已验证）；GetVisibleEntities 查询验证
    auto visible = tw->world->GetVisibleEntities(1);
    bool sees_b = false;
    bool sees_c = false;
    for (uint64_t id : visible) {
        EXPECT_FALSE(id == 1);  // 不含自己
        if (id == 2) sees_b = true;
        if (id == 3) sees_c = true;
    }
    EXPECT_TRUE(sees_b);
    EXPECT_FALSE(sees_c);
}

// ---- 重连/脏属性 ----

GAME_TEST(AoiMapTest, ReconnectRestoresWatcher) {
    // 断线 LeaveMap 后 IsWatcher=false；重连 EnterMap 后 IsWatcher=true
    auto tw = TestWorld::Create();
    auto a = tw->SpawnPlayer(1, 0.0f, 0.0f);
    EXPECT_TRUE(tw->world->IsWatcher(1));
    tw->world->LeaveMap(a);  // 断线
    EXPECT_FALSE(tw->world->IsWatcher(1));
    EXPECT_FALSE(a->IsInMap());
    // 重连：重新 EnterMap（断线时 UnregisterEntity，需重新注册）
    tw->world->EnterMap(a);
    EXPECT_TRUE(tw->world->IsWatcher(1));
    EXPECT_TRUE(a->IsInMap());
}

GAME_TEST(AoiMapTest, MoveWatcherOldCenterCorrect) {
    auto tw = TestWorld::Create();
    auto a = tw->SpawnPlayer(1, 0.0f, 0.0f);
    auto b = tw->SpawnPlayer(2, 1000.0f, 0.0f);  // AOI格125，远处
    tw->counters.clear();
    // B 移动到 A 附近 → 应触发 A 看到 B（appear）
    tw->world->MoveEntity(b, JPH::Vec3(1.0f, 0.0f, 1.0f), EntityPropertyType::kMove);
    EXPECT_TRUE(tw->counters[1].appear >= 1);
    tw->counters.clear();
    // B 再远离 → 应触发 A 收到 disappear（验证 old_center 正确，差集不漏）
    tw->world->MoveEntity(b, JPH::Vec3(1000.0f, 0.0f, 0.0f), EntityPropertyType::kMove);
    EXPECT_TRUE(tw->counters[1].disappear >= 1);
}

GAME_TEST(AoiMapTest, BornPosMoveAwayAndBackReappear) {
    // 协议测试场景：A/B 同出生点 (333,18,415.45)，B 走远再移回
    constexpr float kBornX = 333.0f;
    constexpr float kBornY = 18.0f;
    constexpr float kBornZ = 415.45f;
    auto tw = TestWorld::Create();
    EntitySpawn spawn_a;
    spawn_a.position = JPH::Vec3(kBornX, kBornY, kBornZ);
    auto a = tw->world->SpawnOnMap(EntityType::kPlayer, spawn_a);
    EntitySpawn spawn_b;
    spawn_b.position = JPH::Vec3(kBornX, kBornY, kBornZ);
    auto b = tw->world->SpawnOnMap(EntityType::kPlayer, spawn_b);
    tw->counters.clear();
    tw->world->MoveEntity(b, JPH::Vec3(1000.0f, 20.0f, 1000.0f), EntityPropertyType::kMove);
    EXPECT_TRUE(tw->counters[1].disappear >= 1);
    tw->counters.clear();
    tw->world->MoveEntity(b, JPH::Vec3(kBornX, kBornY, kBornZ), EntityPropertyType::kMove);
    EXPECT_TRUE(tw->counters[1].appear >= 1);
}

GAME_TEST(AoiMapTest, DirtyFlushPushesUpdate) {
    // SetPropertyDirty(sync_immediately=false) → Tick Flush 后观察者收到 update
    auto tw = TestWorld::Create();
    auto a = tw->SpawnPlayer(1, 0.0f, 0.0f);
    auto b = tw->SpawnPlayer(2, 1.0f, 0.0f);
    tw->counters.clear();
    a->SetPropertyDirty(EntityPropertyType::kMove, /*sync_immediately=*/false);
    tw->world->Tick();  // FlushDirty → 触发 update 回调
    EXPECT_TRUE(tw->counters[2].update >= 1);
}

GAME_TEST(AoiMapTest, DirtyFlushDoesNotPushSelfUpdate) {
    // subject 自己也在 receivers_ 时，Flush 不得向自身推 kUpdate
    auto tw = TestWorld::Create();
    auto a = tw->SpawnPlayer(1, 0.0f, 0.0f);
    auto b = tw->SpawnPlayer(2, 1.0f, 0.0f);
    tw->counters.clear();
    a->SetPropertyDirty(EntityPropertyType::kMove, /*sync_immediately=*/false);
    tw->world->Tick();
    EXPECT_EQ(tw->counters[1].update, 0u);
    EXPECT_TRUE(tw->counters[2].update >= 1);
}

GAME_TEST(AoiMapTest, ClearPropertyTypesAfterFlush) {
    // FlushDirty 后 subject 的 dirty_property_types_ 被清空
    auto tw = TestWorld::Create();
    auto a = tw->SpawnPlayer(1, 0.0f, 0.0f);
    auto b = tw->SpawnPlayer(2, 1.0f, 0.0f);
    a->SetPropertyDirty(EntityPropertyType::kMove, /*sync_immediately=*/false);
    EXPECT_TRUE(a->IsDirty());
    tw->world->Tick();
    EXPECT_FALSE(a->IsDirty());  // Flush 后清空
}

GAME_TEST(AoiMapTest, NeedsAoiWatcherOnEntity) {
    // NeedsAoiWatcher 现在是 Entity 成员：玩家 true，非玩家 false
    auto tw = TestWorld::Create();
    auto a = tw->SpawnPlayer(1, 0.0f, 0.0f);
    EXPECT_TRUE(a->NeedsAoiWatcher());
    // 非玩家实体（Town）不需要观察者
    EntitySpawn town_spawn;
    town_spawn.position = JPH::Vec3(0.0f, 0.0f, 0.0f);
    auto town = tw->world->SpawnOnMap(EntityType::kTown, town_spawn);
    EXPECT_FALSE(town->NeedsAoiWatcher());
}

GAME_TEST(AoiMapTest, GetGridCenterOnEntity) {
    // GetGridCenter 是 Entity 成员，返回格中心
    auto tw = TestWorld::Create();
    auto a = tw->SpawnPlayer(1, 0.0f, 0.0f);
    auto center = a->GetGridCenter();
    // 世界 (0,0,0) 落在地图格 0，格中心 = kGridSize/2
    EXPECT_EQ(static_cast<float>(center.GetX()), static_cast<float>(kGridSize) * 0.5f);
    EXPECT_EQ(static_cast<float>(center.GetY()), static_cast<float>(kGridSize) * 0.5f);
    EXPECT_EQ(static_cast<float>(center.GetZ()), static_cast<float>(kGridSize) * 0.5f);
}

GAME_TEST(AoiMapTest, UnregisterOnLeave) {
    // LeaveMap 后实体从注册表移除
    auto tw = TestWorld::Create();
    auto a = tw->SpawnPlayer(1, 0.0f, 0.0f);
    EXPECT_EQ(tw->world->GetEntityCount(), static_cast<size_t>(1));
    tw->world->LeaveMap(a);
    EXPECT_EQ(tw->world->GetEntityCount(), static_cast<size_t>(0));
    EXPECT_EQ(tw->world->FindEntity(1), nullptr);
}
