#include "ai/behavior_tree/monster_ai_nodes.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "ai/behavior_tree/monster_ai_context.h"
#include "ecs/entity/entity.h"
#include "ecs/systems/map_system.h"
#include "ecs/systems/move_system.h"
#include "ecs/systems/world_system.h"

namespace game::ai {
namespace {

constexpr char kContextKey[] = "monster_context";
constexpr float kDistanceEpsilon = 1e-5f;

std::shared_ptr<MonsterAiContext> ContextOf(const BT::TreeNode& node) {
  if (!node.config().blackboard) {
    return nullptr;
  }
  return node.config().blackboard->get<std::shared_ptr<MonsterAiContext>>(
      kContextKey);
}

float DistanceSquared(const EntityPtr& lhs, const EntityPtr& rhs) {
  if (!lhs || !rhs) {
    return std::numeric_limits<float>::max();
  }
  return (lhs->GetPosition() - rhs->GetPosition()).LengthSq();
}

float DistanceSquared(const Vector3D& lhs, const Vector3D& rhs) {
  return (lhs - rhs).LengthSq();
}

bool IsUsableTarget(const std::shared_ptr<MonsterAiContext>& context,
                    const EntityPtr& target, float max_range) {
  const EntityPtr monster = context ? context->monster.lock() : nullptr;
  if (!monster || !target || monster == target || !monster->IsInMap() ||
      !target->IsInMap()) {
    return false;
  }
  return DistanceSquared(monster, target) <= max_range * max_range;
}

EntityPtr FindNearestPlayer(const std::shared_ptr<MonsterAiContext>& context) {
  const EntityPtr monster = context ? context->monster.lock() : nullptr;
  if (!context || !context->world || !monster) {
    return nullptr;
  }

  const Vector3D origin = monster->GetPosition();
  const float aggro_range = std::max(0.0f, context->config.aggro_range);
  const int32_t cell_radius = static_cast<int32_t>(
      std::ceil(aggro_range / static_cast<float>(kGridSize)));

  EntityPtr nearest;
  float nearest_distance_sq = aggro_range * aggro_range;
  context->world->Map().CollectEntitiesInGridBox(
      origin.GridX() - cell_radius, origin.GridY() - cell_radius,
      origin.GridZ() - cell_radius, origin.GridX() + cell_radius + 1,
      origin.GridY() + cell_radius + 1, origin.GridZ() + cell_radius + 1,
      [&](const EntityPtr& candidate) {
        if (!candidate || !candidate->IsPlayer() || !candidate->IsInMap()) {
          return;
        }
        const float distance_sq = DistanceSquared(monster, candidate);
        if (distance_sq > nearest_distance_sq) {
          return;
        }
        if (!nearest ||
            distance_sq < nearest_distance_sq - kDistanceEpsilon ||
            (std::abs(distance_sq - nearest_distance_sq) <=
                 kDistanceEpsilon &&
             candidate->GetId() < nearest->GetId())) {
          nearest_distance_sq = distance_sq;
          nearest = candidate;
        }
      });
  return nearest;
}

void CancelMonsterMove(const std::shared_ptr<MonsterAiContext>& context) {
  const EntityPtr monster = context ? context->monster.lock() : nullptr;
  if (context && context->world && monster &&
      context->world->Move().IsMoving(monster)) {
    context->world->Move().CancelMove(monster, MoveStopReason::kMarchAi);
  }
}

bool IsAtHome(const std::shared_ptr<MonsterAiContext>& context) {
  const EntityPtr monster = context ? context->monster.lock() : nullptr;
  if (!context || !monster) {
    return false;
  }
  const float tolerance = context->config.return_tolerance;
  return DistanceSquared(monster->GetPosition(), context->home_position) <=
         tolerance * tolerance;
}

bool IsOutsideLeash(const std::shared_ptr<MonsterAiContext>& context) {
  const EntityPtr monster = context ? context->monster.lock() : nullptr;
  if (!context || !monster) {
    return false;
  }
  const float leash_range = context->config.leash_range;
  return DistanceSquared(monster->GetPosition(), context->home_position) >
         leash_range * leash_range;
}

bool RequestChaseMove(const std::shared_ptr<MonsterAiContext>& context,
                      const EntityPtr& monster, const EntityPtr& target) {
  if (!context || !context->world || !monster || !target) {
    return false;
  }
  context->chase_destination = target->GetPosition();
  context->chase_destination_valid = true;
  return context->world->Move().RequestMoveTo(monster,
                                              context->chase_destination);
}

BT::NodeStatus ExecuteAttack(
    const std::shared_ptr<MonsterAiContext>& context) {
  const EntityPtr monster = context ? context->monster.lock() : nullptr;
  const EntityPtr target = context ? context->target.lock() : nullptr;
  if (!context || !monster || !context->attack_handler ||
      !IsUsableTarget(context, target, context->config.attack_range)) {
    if (context) {
      context->state = MonsterAiState::kIdle;
    }
    return BT::NodeStatus::FAILURE;
  }

  bool accepted = false;
  try {
    accepted = context->attack_handler(monster, target);
  } catch (...) {
    // 业务攻击回调不能让异常穿透世界 Tick。
    accepted = false;
  }
  if (!accepted) {
    context->state = MonsterAiState::kIdle;
    return BT::NodeStatus::FAILURE;
  }

  context->attack_cooldown_remaining = context->config.attack_cooldown;
  context->state = context->attack_cooldown_remaining > 0.0f
                       ? MonsterAiState::kAttackCooldown
                       : MonsterAiState::kIdle;
  return BT::NodeStatus::SUCCESS;
}

BT::NodeStatus BeginAttackWindup(
    const std::shared_ptr<MonsterAiContext>& context) {
  if (!context) {
    return BT::NodeStatus::FAILURE;
  }
  if (context->attack_cooldown_remaining > 0.0f) {
    context->state = MonsterAiState::kAttackCooldown;
    return BT::NodeStatus::RUNNING;
  }

  context->attack_windup_remaining = context->config.attack_windup;
  context->state = MonsterAiState::kAttackWindup;
  if (context->attack_windup_remaining > 0.0f) {
    return BT::NodeStatus::RUNNING;
  }
  return ExecuteAttack(context);
}

}  // namespace

AcquireTarget::AcquireTarget(const std::string& name,
                             const BT::NodeConfig& config)
    : BT::SyncActionNode(name, config) {}

BT::NodeStatus AcquireTarget::tick() {
  const auto context = ContextOf(*this);
  if (!context || context->state == MonsterAiState::kReturningHome) {
    return BT::NodeStatus::FAILURE;
  }

  EntityPtr target = context->target.lock();
  if (IsUsableTarget(context, target, context->config.aggro_range)) {
    return BT::NodeStatus::SUCCESS;
  }

  const EntityPtr monster = context->monster.lock();
  target = context->target_selector
               ? context->target_selector(monster, context->config.aggro_range)
               : FindNearestPlayer(context);
  if (!IsUsableTarget(context, target, context->config.aggro_range)) {
    context->target.reset();
    return BT::NodeStatus::FAILURE;
  }

  context->target = target;
  return BT::NodeStatus::SUCCESS;
}

IsTargetInAttackRange::IsTargetInAttackRange(
    const std::string& name, const BT::NodeConfig& config)
    : BT::ConditionNode(name, config) {}

BT::NodeStatus IsTargetInAttackRange::tick() {
  const auto context = ContextOf(*this);
  const EntityPtr target = context ? context->target.lock() : nullptr;
  return IsUsableTarget(context, target, context->config.attack_range)
             ? BT::NodeStatus::SUCCESS
             : BT::NodeStatus::FAILURE;
}

ShouldReturnHome::ShouldReturnHome(const std::string& name,
                                   const BT::NodeConfig& config)
    : BT::ConditionNode(name, config) {}

BT::NodeStatus ShouldReturnHome::tick() {
  const auto context = ContextOf(*this);
  const EntityPtr monster = context ? context->monster.lock() : nullptr;
  if (!context || !monster || !monster->IsInMap()) {
    return BT::NodeStatus::FAILURE;
  }
  if (context->state == MonsterAiState::kReturningHome) {
    return BT::NodeStatus::SUCCESS;
  }
  return IsOutsideLeash(context)
             ? BT::NodeStatus::SUCCESS
             : BT::NodeStatus::FAILURE;
}

ReturnHome::ReturnHome(const std::string& name,
                       const BT::NodeConfig& config)
    : BT::StatefulActionNode(name, config) {}

BT::NodeStatus ReturnHome::onStart() {
  const auto context = ContextOf(*this);
  const EntityPtr monster = context ? context->monster.lock() : nullptr;
  if (!context || !context->world || !monster || !monster->IsInMap()) {
    return BT::NodeStatus::FAILURE;
  }

  context->target.reset();
  context->chase_destination_valid = false;
  context->attack_windup_remaining = 0.0f;
  context->attack_cooldown_remaining = 0.0f;
  context->state = MonsterAiState::kReturningHome;
  CancelMonsterMove(context);
  if (IsAtHome(context)) {
    context->state = MonsterAiState::kIdle;
    return BT::NodeStatus::SUCCESS;
  }
  return context->world->Move().RequestMoveTo(monster,
                                              context->home_position)
             ? BT::NodeStatus::RUNNING
             : BT::NodeStatus::FAILURE;
}

BT::NodeStatus ReturnHome::onRunning() {
  const auto context = ContextOf(*this);
  const EntityPtr monster = context ? context->monster.lock() : nullptr;
  if (!context || !context->world || !monster || !monster->IsInMap()) {
    return BT::NodeStatus::FAILURE;
  }
  if (IsAtHome(context)) {
    CancelMonsterMove(context);
    context->state = MonsterAiState::kIdle;
    return BT::NodeStatus::SUCCESS;
  }
  if (context->world->Move().IsMoving(monster)) {
    return BT::NodeStatus::RUNNING;
  }
  return context->world->Move().RequestMoveTo(monster,
                                              context->home_position)
             ? BT::NodeStatus::RUNNING
             : BT::NodeStatus::FAILURE;
}

void ReturnHome::onHalted() {
  CancelMonsterMove(ContextOf(*this));
}

ChaseTarget::ChaseTarget(const std::string& name,
                         const BT::NodeConfig& config)
    : BT::StatefulActionNode(name, config) {}

BT::NodeStatus ChaseTarget::onStart() {
  const auto context = ContextOf(*this);
  const EntityPtr monster = context ? context->monster.lock() : nullptr;
  const EntityPtr target = context ? context->target.lock() : nullptr;
  if (context && (context->state == MonsterAiState::kReturningHome ||
                  IsOutsideLeash(context))) {
    CancelMonsterMove(context);
    context->target.reset();
    context->state = MonsterAiState::kReturningHome;
    return BT::NodeStatus::FAILURE;
  }
  if (!context || !context->world || !monster ||
      !IsUsableTarget(context, target, context->config.aggro_range)) {
    return BT::NodeStatus::FAILURE;
  }
  if (IsUsableTarget(context, target, context->config.attack_range)) {
    context->state = MonsterAiState::kIdle;
    return BT::NodeStatus::SUCCESS;
  }

  context->state = MonsterAiState::kChasing;
  const bool accepted = RequestChaseMove(context, monster, target);
  return accepted ? BT::NodeStatus::RUNNING : BT::NodeStatus::FAILURE;
}

BT::NodeStatus ChaseTarget::onRunning() {
  const auto context = ContextOf(*this);
  const EntityPtr monster = context ? context->monster.lock() : nullptr;
  const EntityPtr target = context ? context->target.lock() : nullptr;
  if (context && IsOutsideLeash(context)) {
    CancelMonsterMove(context);
    context->target.reset();
    context->state = MonsterAiState::kReturningHome;
    return BT::NodeStatus::FAILURE;
  }
  if (!context || !context->world || !monster ||
      !IsUsableTarget(context, target, context->config.aggro_range)) {
    CancelMonsterMove(context);
    if (context) {
      context->target.reset();
      context->chase_destination_valid = false;
      context->state = MonsterAiState::kIdle;
    }
    return BT::NodeStatus::FAILURE;
  }
  if (IsUsableTarget(context, target, context->config.attack_range)) {
    CancelMonsterMove(context);
    context->state = MonsterAiState::kIdle;
    return BT::NodeStatus::SUCCESS;
  }

  const float repath_distance = context->config.chase_repath_distance;
  const bool target_moved =
      !context->chase_destination_valid ||
      DistanceSquared(target->GetPosition(), context->chase_destination) >=
          repath_distance * repath_distance;
  if (target_moved || !context->world->Move().IsMoving(monster)) {
    if (!RequestChaseMove(context, monster, target)) {
      context->state = MonsterAiState::kIdle;
      return BT::NodeStatus::FAILURE;
    }
  }
  context->state = MonsterAiState::kChasing;
  return BT::NodeStatus::RUNNING;
}

void ChaseTarget::onHalted() {
  const auto context = ContextOf(*this);
  CancelMonsterMove(context);
  if (context && context->state == MonsterAiState::kChasing) {
    context->state = MonsterAiState::kIdle;
  }
}

AttackTarget::AttackTarget(const std::string& name,
                           const BT::NodeConfig& config)
    : BT::StatefulActionNode(name, config) {}

BT::NodeStatus AttackTarget::onStart() {
  const auto context = ContextOf(*this);
  const EntityPtr monster = context ? context->monster.lock() : nullptr;
  const EntityPtr target = context ? context->target.lock() : nullptr;
  if (context && IsOutsideLeash(context)) {
    context->target.reset();
    context->state = MonsterAiState::kReturningHome;
    return BT::NodeStatus::FAILURE;
  }
  if (!context || !monster ||
      !IsUsableTarget(context, target, context->config.attack_range)) {
    if (context) {
      context->target.reset();
      context->state = MonsterAiState::kIdle;
    }
    return BT::NodeStatus::FAILURE;
  }

  CancelMonsterMove(context);
  return BeginAttackWindup(context);
}

BT::NodeStatus AttackTarget::onRunning() {
  const auto context = ContextOf(*this);
  const EntityPtr monster = context ? context->monster.lock() : nullptr;
  const EntityPtr target = context ? context->target.lock() : nullptr;
  if (context && IsOutsideLeash(context)) {
    context->attack_windup_remaining = 0.0f;
    context->target.reset();
    context->state = MonsterAiState::kReturningHome;
    return BT::NodeStatus::FAILURE;
  }
  if (!context || !monster ||
      !IsUsableTarget(context, target, context->config.attack_range)) {
    if (context) {
      context->attack_windup_remaining = 0.0f;
      context->target.reset();
      context->state = MonsterAiState::kIdle;
    }
    return BT::NodeStatus::FAILURE;
  }
  CancelMonsterMove(context);

  if (context->state == MonsterAiState::kAttackCooldown) {
    return context->attack_cooldown_remaining > 0.0f
               ? BT::NodeStatus::RUNNING
               : BeginAttackWindup(context);
  }
  if (context->state != MonsterAiState::kAttackWindup) {
    return BeginAttackWindup(context);
  }
  return context->attack_windup_remaining > 0.0f
             ? BT::NodeStatus::RUNNING
             : ExecuteAttack(context);
}

void AttackTarget::onHalted() {
  const auto context = ContextOf(*this);
  if (!context) {
    return;
  }
  context->attack_windup_remaining = 0.0f;
  if (context->state == MonsterAiState::kAttackWindup ||
      context->state == MonsterAiState::kAttackCooldown) {
    context->state = MonsterAiState::kIdle;
  }
}

Idle::Idle(const std::string& name, const BT::NodeConfig& config)
    : BT::SyncActionNode(name, config) {}

BT::NodeStatus Idle::tick() {
  const auto context = ContextOf(*this);
  if (context && context->state != MonsterAiState::kReturningHome) {
    context->state = MonsterAiState::kIdle;
  }
  return BT::NodeStatus::SUCCESS;
}

void RegisterMonsterAiNodes(BT::BehaviorTreeFactory& factory) {
  factory.registerNodeType<AcquireTarget>("AcquireTarget");
  factory.registerNodeType<IsTargetInAttackRange>("IsTargetInAttackRange");
  factory.registerNodeType<ShouldReturnHome>("ShouldReturnHome");
  factory.registerNodeType<ReturnHome>("ReturnHome");
  factory.registerNodeType<ChaseTarget>("ChaseTarget");
  factory.registerNodeType<AttackTarget>("AttackTarget");
  factory.registerNodeType<Idle>("Idle");
}

}  // namespace game::ai
