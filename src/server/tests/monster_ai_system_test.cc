#include "test_harness.h"

#include <algorithm>

#include "ai/behavior_tree/monster_ai_system.h"
#include "ecs/systems/world_system.h"
#include "test_map_invariants.h"

namespace {

constexpr float kAiTestDt = 0.05f;

template <typename Predicate>
bool TickUntil(const std::shared_ptr<WorldSystem>& world, int max_steps,
               Predicate predicate, float dt = kAiTestDt) {
  if (predicate()) {
    return true;
  }
  for (int step = 0; step < max_steps; ++step) {
    world->Tick(dt);
    if (predicate()) {
      return true;
    }
  }
  return false;
}

float EntityDistance(const EntityPtr& lhs, const EntityPtr& rhs) {
  return (lhs->GetPosition() - rhs->GetPosition()).Length();
}

}  // namespace

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
  config.attack_windup = 0.1f;
  config.attack_cooldown = 0.2f;
  config.tick_interval = kAiTestDt;
  EXPECT_TRUE(world->MonsterAi().AttachMonster(monster, config));

  EXPECT_TRUE(TickUntil(world, 40, [&] { return attack_count > 0; }));
  EXPECT_EQ(attack_count, 1);
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
  config.attack_windup = 0.05f;
  config.attack_cooldown = 0.2f;
  config.tick_interval = kAiTestDt;
  EXPECT_TRUE(world->MonsterAi().AttachMonster(monster, config));

  EXPECT_TRUE(TickUntil(world, 80, [&] { return attack_count > 0; }));
  EXPECT_EQ(attack_count, 1);
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

GAME_TEST(MonsterAiSystemTest, AttackUsesWindupAndCooldown) {
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
  config.attack_windup = 0.30f;
  config.attack_cooldown = 0.50f;
  config.tick_interval = kAiTestDt;
  EXPECT_TRUE(world->MonsterAi().AttachMonster(monster, config));

  const bool entered_windup = TickUntil(world, 20, [&] {
    const auto snapshot = world->MonsterAi().InspectMonster(monster->GetId());
    return snapshot &&
           snapshot->state == game::ai::MonsterAiState::kAttackWindup;
  });
  EXPECT_TRUE(entered_windup);
  EXPECT_EQ(attack_count, 0);

  // 前摇尚未完成时不得提前结算攻击。
  world->Tick(0.10f);
  EXPECT_EQ(attack_count, 0);

  EXPECT_TRUE(TickUntil(world, 20, [&] { return attack_count == 1; }));
  const auto cooldown_snapshot =
      world->MonsterAi().InspectMonster(monster->GetId());
  EXPECT_TRUE(cooldown_snapshot.has_value());
  EXPECT_TRUE(cooldown_snapshot->state ==
              game::ai::MonsterAiState::kAttackCooldown);
  EXPECT_GT(cooldown_snapshot->attack_cooldown_remaining, 0.0f);

  // 冷却仍有明显余量时，不允许开始第二次攻击结算。
  const float safe_cooldown_step =
      cooldown_snapshot->attack_cooldown_remaining * 0.5f;
  world->Tick(safe_cooldown_step);
  EXPECT_EQ(attack_count, 1);

  EXPECT_TRUE(TickUntil(world, 40, [&] { return attack_count >= 2; }));
}

GAME_TEST(MonsterAiSystemTest, AttackRequiresHandler) {
  auto world = test::MakeWorld();
  const EntityPtr monster = world->SpawnOnMap(
      EntityType::kMarch, EntitySpawn::At(Vector3D(2.0f, 2.0f, 2.0f)));
  const EntityPtr player = world->SpawnOnMap(
      EntityType::kPlayer, EntitySpawn::At(Vector3D(3.0f, 2.0f, 2.0f)));

  game::ai::MonsterAiConfig config;
  config.aggro_range = 10.0f;
  config.attack_range = 2.0f;
  config.attack_windup = 0.10f;
  config.attack_cooldown = 0.20f;
  config.tick_interval = kAiTestDt;
  EXPECT_TRUE(world->MonsterAi().AttachMonster(monster, config));

  bool entered_cooldown_without_handler = false;
  for (int step = 0; step < 20; ++step) {
    world->Tick(kAiTestDt);
    const auto snapshot = world->MonsterAi().InspectMonster(monster->GetId());
    EXPECT_TRUE(snapshot.has_value());
    if (snapshot->state == game::ai::MonsterAiState::kAttackCooldown) {
      entered_cooldown_without_handler = true;
      break;
    }
  }
  EXPECT_FALSE(entered_cooldown_without_handler);

  int attack_count = 0;
  world->MonsterAi().SetAttackHandler(
      [&](const EntityPtr&, const EntityPtr&) {
        ++attack_count;
        return true;
      });
  EXPECT_TRUE(TickUntil(world, 20, [&] { return attack_count > 0; }));
}

GAME_TEST(MonsterAiSystemTest, ReturnsHomeAfterExceedingLeash) {
  auto world = test::MakeWorld();
  const EntityPtr monster = world->SpawnOnMap(
      EntityType::kMarch, EntitySpawn::At(Vector3D(2.0f, 2.0f, 2.0f)));
  const EntityPtr player = world->SpawnOnMap(
      EntityType::kPlayer, EntitySpawn::At(Vector3D(12.0f, 2.0f, 2.0f)));
  const Vector3D home_position = monster->GetPosition();

  bool allow_target = true;
  world->MonsterAi().SetTargetSelector(
      [&](const EntityPtr&, float) { return allow_target ? player : nullptr; });
  int attack_count = 0;
  world->MonsterAi().SetAttackHandler(
      [&](const EntityPtr&, const EntityPtr&) {
        ++attack_count;
        return true;
      });

  game::ai::MonsterAiConfig config;
  config.aggro_range = 20.0f;
  config.attack_range = 0.5f;
  config.move_speed = 5.0f;
  config.leash_range = 3.0f;
  config.return_tolerance = 0.15f;
  config.attack_windup = 0.05f;
  config.attack_cooldown = 0.20f;
  config.tick_interval = kAiTestDt;
  EXPECT_TRUE(world->MonsterAi().AttachMonster(monster, config));

  float max_home_distance = 0.0f;
  const bool started_returning = TickUntil(world, 80, [&] {
    max_home_distance =
        std::max(max_home_distance,
                 (monster->GetPosition() - home_position).Length());
    const auto snapshot = world->MonsterAi().InspectMonster(monster->GetId());
    return snapshot &&
           snapshot->state == game::ai::MonsterAiState::kReturningHome;
  });
  EXPECT_TRUE(started_returning);
  EXPECT_GE(max_home_distance,
            config.leash_range - config.move_speed * kAiTestDt);

  const auto returning_snapshot =
      world->MonsterAi().InspectMonster(monster->GetId());
  EXPECT_TRUE(returning_snapshot.has_value());
  EXPECT_EQ(returning_snapshot->target_entity_id, 0u);

  // 防止回到出生点后再次获取同一玩家，专注验证完整返航过程。
  allow_target = false;
  const bool returned_home = TickUntil(world, 80, [&] {
    const auto snapshot = world->MonsterAi().InspectMonster(monster->GetId());
    return snapshot && snapshot->state == game::ai::MonsterAiState::kIdle &&
           (monster->GetPosition() - home_position).Length() <=
               config.return_tolerance;
  });
  if (!returned_home) {
    const auto snapshot = world->MonsterAi().InspectMonster(monster->GetId());
    std::cerr << "ReturnHome diagnostic: state="
              << (snapshot ? static_cast<int>(snapshot->state) : -1)
              << " distance="
              << (monster->GetPosition() - home_position).Length()
              << " moving=" << world->Move().IsMoving(monster) << std::endl;
  }
  EXPECT_TRUE(returned_home);
  EXPECT_EQ(attack_count, 0);
  EXPECT_FALSE(world->Move().IsMoving(monster));
}

GAME_TEST(MonsterAiSystemTest, EqualDistanceTargetsUseSmallestEntityId) {
  auto world = test::MakeWorld();
  const EntityPtr monster = world->SpawnOnMap(
      EntityType::kMarch, EntitySpawn::At(Vector3D(2.0f, 2.0f, 2.0f)));
  const EntityPtr first_player = world->SpawnOnMap(
      EntityType::kPlayer, EntitySpawn::At(Vector3D(7.0f, 2.0f, 2.0f)));
  const EntityPtr second_player = world->SpawnOnMap(
      EntityType::kPlayer, EntitySpawn::At(Vector3D(2.0f, 7.0f, 2.0f)));
  EXPECT_LT(first_player->GetId(), second_player->GetId());
  EXPECT_NEAR(EntityDistance(monster, first_player),
              EntityDistance(monster, second_player), 0.001f);

  uint64_t attacked_target_id = 0;
  world->MonsterAi().SetAttackHandler(
      [&](const EntityPtr&, const EntityPtr& target) {
        attacked_target_id = target->GetId();
        return true;
      });

  game::ai::MonsterAiConfig config;
  config.aggro_range = 10.0f;
  config.attack_range = 6.0f;
  config.attack_windup = 0.05f;
  config.attack_cooldown = 0.20f;
  config.tick_interval = kAiTestDt;
  EXPECT_TRUE(world->MonsterAi().AttachMonster(monster, config));

  EXPECT_TRUE(TickUntil(world, 40, [&] { return attacked_target_id != 0; }));
  EXPECT_EQ(attacked_target_id, first_player->GetId());
}

GAME_TEST(MonsterAiSystemTest, TargetLeavingMapClearsCurrentTarget) {
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
  config.attack_range = 0.5f;
  config.move_speed = 5.0f;
  config.leash_range = 30.0f;
  config.return_tolerance = 0.1f;
  config.attack_windup = 0.05f;
  config.attack_cooldown = 0.20f;
  config.tick_interval = kAiTestDt;
  EXPECT_TRUE(world->MonsterAi().AttachMonster(monster, config));

  EXPECT_TRUE(TickUntil(world, 20, [&] {
    const auto snapshot = world->MonsterAi().InspectMonster(monster->GetId());
    return snapshot &&
           snapshot->state == game::ai::MonsterAiState::kChasing;
  }));

  world->LeaveMap(player);
  EXPECT_TRUE(TickUntil(world, 20, [&] {
    const auto snapshot = world->MonsterAi().InspectMonster(monster->GetId());
    return snapshot && snapshot->target_entity_id == 0u &&
           snapshot->state != game::ai::MonsterAiState::kChasing &&
           snapshot->state != game::ai::MonsterAiState::kAttackWindup &&
           snapshot->state != game::ai::MonsterAiState::kAttackCooldown;
  }));
  EXPECT_EQ(attack_count, 0);
}

GAME_TEST(MonsterAiSystemTest, ReattachAndLeaveMapAreIdempotent) {
  auto world = test::MakeWorld();
  const EntityPtr monster = world->SpawnOnMap(
      EntityType::kMarch, EntitySpawn::At(Vector3D(2.0f, 2.0f, 2.0f)));
  const EntityPtr player = world->SpawnOnMap(
      EntityType::kPlayer, EntitySpawn::At(Vector3D(10.0f, 2.0f, 2.0f)));

  game::ai::MonsterAiConfig first_config;
  first_config.aggro_range = 20.0f;
  first_config.attack_range = 0.5f;
  first_config.move_speed = 4.0f;
  first_config.leash_range = 30.0f;
  first_config.tick_interval = kAiTestDt;
  EXPECT_TRUE(world->MonsterAi().AttachMonster(monster, first_config));
  EXPECT_TRUE(TickUntil(world, 20,
                        [&] { return world->Move().IsMoving(monster); }));

  game::ai::MonsterAiConfig second_config = first_config;
  second_config.move_speed = 7.0f;
  EXPECT_TRUE(world->MonsterAi().AttachMonster(monster, second_config));
  EXPECT_TRUE(world->MonsterAi().AttachMonster(monster, second_config));
  EXPECT_EQ(world->MonsterAi().MonsterCount(), 1u);
  EXPECT_TRUE(world->MonsterAi().HasMonster(monster->GetId()));
  EXPECT_FALSE(world->Move().IsMoving(monster));
  EXPECT_NEAR(monster->Move()->GetCurSpeed(), second_config.move_speed, 0.001f);

  const auto snapshot = world->MonsterAi().InspectMonster(monster->GetId());
  EXPECT_TRUE(snapshot.has_value());
  EXPECT_TRUE(snapshot->state == game::ai::MonsterAiState::kIdle);
  EXPECT_EQ(snapshot->target_entity_id, 0u);

  world->LeaveMap(monster);
  world->LeaveMap(monster);
  world->MonsterAi().DetachMonster(monster->GetId());
  EXPECT_EQ(world->MonsterAi().MonsterCount(), 0u);
  EXPECT_FALSE(world->MonsterAi().HasMonster(monster->GetId()));
  EXPECT_FALSE(world->MonsterAi().InspectMonster(monster->GetId()).has_value());
  EXPECT_TRUE(player->IsInMap());
}
