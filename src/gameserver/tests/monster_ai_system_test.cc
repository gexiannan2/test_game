#include "test_harness.h"

#include "ai/behavior_tree/monster_ai_system.h"
#include "ecs/systems/world_system.h"
#include "test_map_invariants.h"

GAME_TEST_SUITE(MonsterAiSystemTest)

GAME_TEST(MonsterAiSystemTest, AttacksPlayerAlreadyInRange) {
  auto world = test::MakeWorld();
  const EntityPtr monster = world->SpawnOnMap(
      EntityType::kMarch, EntitySpawn::At(Vector3D(2.0f, 2.0f, 2.0f)));
  const EntityPtr player = world->SpawnOnMap(
      EntityType::kPlayer, EntitySpawn::At(Vector3D(3.0f, 2.0f, 2.0f)));

  int attack_count = 0;
  world->MonsterAi().SetAttackHandler(
      [&](const EntityPtr& attacker, const EntityPtr& target) {
        EXPECT_EQ(attacker->GetId(), monster->GetId());
        EXPECT_EQ(target->GetId(), player->GetId());
        ++attack_count;
        return true;
      });

  game::ai::MonsterAiConfig config;
  config.aggro_range = 10.0f;
  config.attack_range = 2.0f;
  config.tick_interval = 0.1f;
  EXPECT_TRUE(world->MonsterAi().AttachMonster(monster, config));

  // 第一次 Tick 索敌，下一次 Tick 进入攻击分支。
  world->Tick(0.1f);
  world->Tick(0.1f);
  EXPECT_GT(attack_count, 0);
  EXPECT_EQ(world->MonsterAi().MonsterCount(), 1u);

  world->LeaveMap(monster);
  EXPECT_EQ(world->MonsterAi().MonsterCount(), 0u);
}

GAME_TEST(MonsterAiSystemTest, ChasesThenAttacksPlayer) {
  auto world = test::MakeWorld();
  const EntityPtr monster = world->SpawnOnMap(
      EntityType::kMarch, EntitySpawn::At(Vector3D(2.0f, 2.0f, 2.0f)));
  const EntityPtr player = world->SpawnOnMap(
      EntityType::kPlayer, EntitySpawn::At(Vector3D(10.0f, 2.0f, 2.0f)));

  int attack_count = 0;
  world->MonsterAi().SetAttackHandler(
      [&](const EntityPtr&, const EntityPtr&) {
        ++attack_count;
        return true;
      });

  game::ai::MonsterAiConfig config;
  config.aggro_range = 20.0f;
  config.attack_range = 1.0f;
  config.move_speed = 10.0f;
  config.tick_interval = 0.1f;
  EXPECT_TRUE(world->MonsterAi().AttachMonster(monster, config));

  for (int i = 0; i < 20 && attack_count == 0; ++i) {
    world->Tick(0.1f);
  }

  EXPECT_GT(attack_count, 0);
  EXPECT_LE((monster->GetPosition() - player->GetPosition()).Length(),
            config.attack_range);
}

GAME_TEST(MonsterAiSystemTest, RejectsPlayerAsMonster) {
  auto world = test::MakeWorld();
  const EntityPtr player = world->SpawnOnMap(
      EntityType::kPlayer, EntitySpawn::At(Vector3D(2.0f, 2.0f, 2.0f)));

  EXPECT_FALSE(world->MonsterAi().AttachMonster(player));
  EXPECT_EQ(world->MonsterAi().MonsterCount(), 0u);
}
