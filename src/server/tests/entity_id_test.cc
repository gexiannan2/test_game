// WorldSystem 实体 ID 分配器：AllocateEntityId 与 Spawn 共用单调序列。
#include "test_harness.h"

#include "ecs/systems/world_system.h"
#include "test_map_invariants.h"

GAME_TEST_SUITE(EntityIdTest);

GAME_TEST(EntityIdTest, AllocateEntityIdMonotonic) {
    auto world = test::MakeWorld();
    uint64_t a = world->AllocateEntityId();
    uint64_t b = world->AllocateEntityId();
    EXPECT_NE(a, b);
    EXPECT_GT(b, a);
}

GAME_TEST(EntityIdTest, SpawnUsesSameAllocator) {
    auto world = test::MakeWorld();
    EntityPtr e1 = world->Spawn(EntityType::kMarch,
                                EntitySpawn::At(test::GridCenter(1, 1)));
    EntityPtr e2 = world->Spawn(EntityType::kMarch,
                                EntitySpawn::At(test::GridCenter(2, 2)));
    EXPECT_TRUE(e1 != nullptr && e2 != nullptr);
    EXPECT_NE(e1->GetId(), e2->GetId());
}
