#pragma once

#include <memory>

#include "ai/behavior_tree/monster_ai_system.h"
#include "common/vector3d.h"

class WorldSystem;

namespace game::ai {

// 行为树黑板只持有弱实体引用；实体离图或销毁后，节点会自然失败。
struct MonsterAiContext {
  WorldSystem* world = nullptr;
  std::weak_ptr<Entity> monster;
  std::weak_ptr<Entity> target;
  Vector3D home_position;
  Vector3D chase_destination;
  bool chase_destination_valid = false;
  MonsterAiConfig config;
  MonsterAiState state = MonsterAiState::kIdle;
  float attack_windup_remaining = 0.0f;
  float attack_cooldown_remaining = 0.0f;
  MonsterAiSystem::TargetSelector target_selector;
  MonsterAiSystem::AttackHandler attack_handler;
};

}  // namespace game::ai
