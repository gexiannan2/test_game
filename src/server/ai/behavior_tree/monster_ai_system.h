#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

class Entity;
class WorldSystem;

namespace game::ai {

using EntityPtr = std::shared_ptr<::Entity>;

enum class MonsterAiState : uint8_t {
  kIdle = 0,
  kChasing,
  kAttackWindup,
  kAttackCooldown,
  kReturningHome,
};

// 单只怪物的基础行为参数。行为树低频决策，MoveSystem 仍按世界帧率推进位移。
struct MonsterAiConfig {
  float aggro_range = 15.0f;
  float attack_range = 2.0f;
  float leash_range = 30.0f;
  float return_tolerance = 0.25f;
  float move_speed = 5.0f;
  float chase_repath_distance = 1.0f;
  float tick_interval = 0.2f;
  float attack_windup = 0.45f;
  float attack_cooldown = 1.0f;
};

// 只读运行快照，供自动化测试、GM 指令和监控使用。
struct MonsterAiSnapshot {
  MonsterAiState state = MonsterAiState::kIdle;
  uint64_t target_entity_id = 0;
  float attack_cooldown_remaining = 0.0f;
};

// 怪物行为树调度器。树实例由系统集中持有，避免把第三方类型泄漏进 ECS 组件。
class MonsterAiSystem {
 public:
  using TargetSelector =
      std::function<EntityPtr(const EntityPtr& monster, float aggro_range)>;
  using AttackHandler =
      std::function<bool(const EntityPtr& monster, const EntityPtr& target)>;

  MonsterAiSystem();
  ~MonsterAiSystem();

  MonsterAiSystem(const MonsterAiSystem&) = delete;
  MonsterAiSystem& operator=(const MonsterAiSystem&) = delete;

  void Bind(WorldSystem* world);

  // 未注入索敌器时，默认在地图格范围内选择最近的玩家。
  void SetTargetSelector(TargetSelector selector);

  // 战斗系统尚未落地；通过回调把 AttackTarget 节点与未来技能系统隔离。
  void SetAttackHandler(AttackHandler handler);

  bool AttachMonster(const EntityPtr& monster,
                     const MonsterAiConfig& config = {});
  void DetachMonster(uint64_t entity_id);
  bool HasMonster(uint64_t entity_id) const;
  std::size_t MonsterCount() const;
  std::optional<MonsterAiSnapshot> InspectMonster(uint64_t entity_id) const;

  void Tick(float dt);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace game::ai
