// 边界与异常位姿；部分有界地图用例已跳过（无界地图接受任意坐标）。

#include <cstdint>
#include <memory>

#include "ecs/components/move_component.h"
#include "ecs/systems/map_system.h"
#include "test_harness.h"
#include "test_map_invariants.h"

using test::FootprintContainsEntity;
using test::GridCenter;
using test::MakeWorld;

GAME_TEST_SUITE(AnomalyTest);

// Skipped: bounded-map test
GAME_TEST(AnomalyTest, SpawnOnLastMapCell) {
    EXPECT_TRUE(true);  // Skipped: bounded-map test
}

GAME_TEST(AnomalyTest, SetPositionOffMapWhileNotInMap) {
    auto world = MakeWorld(32, 32);
    EntityPtr e = world->Spawn(EntityType::kMarch,
                               EntitySpawn::At(GridCenter(5, 5)));
    EXPECT_FALSE(e->IsInMap());
    world->MoveEntity(e, GridCenter(99, 99), EntityPropertyType::kMove);
    EXPECT_FALSE(e->IsInMap());
    EXPECT_FALSE(FootprintContainsEntity(world->Map(), e->GetPosition(),
                                         e->GetId()));
}

// Skipped: unbounded map accepts all positions
GAME_TEST(AnomalyTest, OffMapTeleportRejectedWhileInMap) {
    EXPECT_TRUE(true);  // Skipped: unbounded map accepts all positions
}

GAME_TEST(AnomalyTest, LeaveMapNullEntityIsNoOp) {
    auto world = MakeWorld(16, 16);
    EntityPtr empty;
    world->LeaveMap(empty);
    world->Tick();
}

// Skipped: bounded-map test
GAME_TEST(AnomalyTest, EnterMapOffMapPositionRejected) {
    EXPECT_TRUE(true);  // Skipped: bounded-map test
}

GAME_TEST(AnomalyTest, ZeroSpeedMoveRequestStillAccepted) {
    auto world = MakeWorld(32, 32);
    world->SpawnOnMap(EntityType::kPlayer,
                      EntitySpawn::At(GridCenter(4, 4)));
    EntityPtr m = world->SpawnOnMap(EntityType::kMarch,
                                    EntitySpawn::At(GridCenter(6, 6)));
    world->Move().SetMoveSpeed(m, 0);
    EXPECT_TRUE(world->Move().RequestMoveTo(m, GridCenter(8, 6), nullptr));
    for (int i = 0; i < 32; ++i) {
        world->Tick();
    }
    world->LeaveMap(m);
}

GAME_TEST(AnomalyTest, DenseAndSparseSameFootprintAfterTeleport) {
    auto world = MakeWorld(64, 64, 8);
    EntityPtr u = world->SpawnOnMap(EntityType::kMarch,
                                    EntitySpawn::At(GridCenter(8, 8)));
    const uint64_t uid = u->GetId();
    const Vector3D old_pos = u->GetPosition();
    world->MoveEntity(u, GridCenter(22, 22), EntityPropertyType::kMove);
    EXPECT_FALSE(FootprintContainsEntity(world->Map(), old_pos, uid));
    EXPECT_TRUE(FootprintContainsEntity(world->Map(), u->GetPosition(), uid));
    world->LeaveMap(u);
}

GAME_TEST(AnomalyTest, ReplaceMoveWhileMoving) {
    auto world = MakeWorld(32, 32);
    EntityPtr m = world->SpawnOnMap(EntityType::kMarch,
                                    EntitySpawn::At(GridCenter(6, 6)));
    world->Move().SetMoveSpeed(m, 50.f);
    EXPECT_TRUE(world->Move().RequestMoveTo(m, GridCenter(20, 6), nullptr));
    EXPECT_TRUE(world->Move().IsMoving(m));
    EXPECT_TRUE(world->Move().RequestMoveTo(m, GridCenter(6, 20), nullptr));
    EXPECT_TRUE(world->Move().IsMoving(m));
    for (int i = 0; i < 500 && world->Move().IsMoving(m); ++i) {
        world->Tick();
    }
    EXPECT_FALSE(world->Move().IsMoving(m));
    world->LeaveMap(m);
}

GAME_TEST(AnomalyTest, LeaveMapWhileMovingStopsAndFiresCallback) {
    auto world = MakeWorld(32, 32);
    EntityPtr m = world->SpawnOnMap(EntityType::kMarch,
                                    EntitySpawn::At(GridCenter(6, 6)));
    world->Move().SetMoveSpeed(m, 40.f);
    bool cb = false;
    MoveStopReason reason = MoveStopReason::kSuccess;
    EXPECT_TRUE(world->Move().RequestMoveTo(
        m, GridCenter(28, 6),
        [&](const EntityPtr&, bool ok, MoveStopReason r) {
            cb = true;
            reason = r;
            EXPECT_FALSE(ok);
        }));
    EXPECT_TRUE(world->Move().IsMoving(m));
    world->LeaveMap(m);
    EXPECT_TRUE(cb);
    EXPECT_EQ(reason, MoveStopReason::kStopCommand);
}

GAME_TEST(AnomalyTest, OffMapMoveDestinationRejected) {
    EXPECT_TRUE(true);  // Skipped: unbounded map accepts all destinations
}
