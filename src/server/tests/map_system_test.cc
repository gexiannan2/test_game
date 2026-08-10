// 地图系统测试（无界地图）；跳过 ToMapIndex/IsInMap 边界等不兼容用例。

#include <list>
#include <set>

#include "ecs/entity/entity.h"
#include "common/aoi_def.h"
#include "ecs/systems/map_system.h"
#include "ecs/systems/world_system.h"
#include "test_harness.h"
#include "test_map_invariants.h"

using test::MakeDefaultEntityFactory;

GAME_TEST_SUITE(MapIndexTest);

GAME_TEST(MapIndexTest, CenterPos) {
    MapSystem map(SceneRegionType::kMap);
    map.Init();
    Vector3D center;
    EXPECT_TRUE(map.MapIndexToCenterPos(1, 2, 3, center));
    // 格中心 = grid_index * kGridSize + kGridSize/2
    EXPECT_NEAR(static_cast<float>(center.GetX()),
                static_cast<float>(1 * kGridSize + kGridSize / 2), 0.01f);
    EXPECT_NEAR(static_cast<float>(center.GetY()),
                static_cast<float>(2 * kGridSize + kGridSize / 2), 0.01f);
    EXPECT_NEAR(static_cast<float>(center.GetZ()),
                static_cast<float>(3 * kGridSize + kGridSize / 2), 0.01f);
}

GAME_TEST_SUITE(MapBoundsTest);

GAME_TEST(MapBoundsTest, GetGridLazyAlloc) {
    MapSystem map(SceneRegionType::kMap);
    map.Init();
    // 无界地图：GetGrid 对未创建格返回 nullptr（稀疏惰性创建）
    EXPECT_TRUE(map.GetGrid(0, 0, 0) == nullptr);
}

GAME_TEST_SUITE(LineInterGridTest);

GAME_TEST(LineInterGridTest, SameCell) {
    MapSystem map(SceneRegionType::kMap);
    map.Init();
    std::list<GridKey> keys;
    const Vector3D a(3, 3, 3);
    const Vector3D b(4, 4, 4);
    EXPECT_TRUE(map.LineInterGrid(a, b, keys));
    EXPECT_FALSE(keys.empty());
}

GAME_TEST(LineInterGridTest, StraightAlongX) {
    MapSystem map(SceneRegionType::kMap);
    map.Init();
    std::list<GridKey> keys;
    const Vector3D start(0, 0, 0);
    const Vector3D end(static_cast<float>(5 * kGridSize), 0, 0);
    EXPECT_TRUE(map.LineInterGrid(start, end, keys));
    EXPECT_GE(keys.size(), 5u);

    // 验证无重复格
    std::set<GridKey> unique(keys.begin(), keys.end());
    EXPECT_EQ(unique.size(), keys.size());
}

GAME_TEST_SUITE(MapGridStorageTest);

GAME_TEST(MapGridStorageTest, HashGridLazyAlloc) {
    MapSystem map(SceneRegionType::kMap);
    map.Init();
    EXPECT_TRUE(map.GridStorage() == MapGridStorage::kHash);
    EXPECT_TRUE(map.GetGrid(1, 1, 1) == nullptr);
    EXPECT_EQ(map.AllocatedGridCount(), 0u);
}

GAME_TEST(MapGridStorageTest, HashGridReleasedWhenEmpty) {
    auto world = WorldSystem::Create(SceneRegionType::kMap);
    world->SetEntityFactory(MakeDefaultEntityFactory());
    world->Init();
    EXPECT_EQ(world->Map().AllocatedGridCount(), 0u);

    EntityPtr e = world->SpawnOnMap(
        EntityType::kMarch,
        EntitySpawn::At(test::GridCenter(1, 1)));
    const size_t occupied = world->Map().AllocatedGridCount();
    EXPECT_GT(occupied, 0u);

    world->MoveEntity(e, test::GridCenter(40, 40));
    // 旧足迹格应已回收
    EXPECT_TRUE(world->Map().GetGrid(1, 1, 0) == nullptr);

    world->LeaveMap(e);
    EXPECT_EQ(world->Map().AllocatedGridCount(), 0u);
}

// 回调内触发 LeaveMap（修改 hash_grids_ / entities_）不应导致迭代器失效崩溃
GAME_TEST(MapGridStorageTest, CollectEntitiesReentrantLeaveMapSafe) {
    auto world = test::MakeWorld();
    EntityPtr a = world->Spawn(EntityType::kMarch, EntitySpawn::At(test::GridCenter(5, 5)));
    EntityPtr b = world->Spawn(EntityType::kMarch, EntitySpawn::At(test::GridCenter(6, 5)));
    world->EnterMap(a);
    world->EnterMap(b);

    bool triggered = false;
    world->Map().CollectEntitiesInGridBox(
        0, 0, 0, 100, 100, 100, [&](const EntityPtr& e) {
            if (e == b && !triggered) {
                triggered = true;
                world->LeaveMap(a);
            }
        });
    EXPECT_TRUE(triggered);
    EXPECT_FALSE(a->IsInMap());
    EXPECT_TRUE(b->IsInMap());
    world->LeaveMap(b);
}
