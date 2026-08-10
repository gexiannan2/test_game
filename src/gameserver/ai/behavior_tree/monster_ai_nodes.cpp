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
        if (distance_sq <= nearest_distance_sq) {
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

}  // namespace

AcquireTarget::AcquireTarget(const std::string& name,
                             const BT::NodeConfig& config)
    : BT::SyncActionNode(name, config) {}

BT::NodeStatus AcquireTarget::tick() {
  const auto context = ContextOf(*this);
  if (!context) {
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

ChaseTarget::ChaseTarget(const std::string& name,
                         const BT::NodeConfig& config)
    : BT::StatefulActionNode(name, config) {}

BT::NodeStatus ChaseTarget::onStart() {
  const auto context = ContextOf(*this);
  const EntityPtr monster = context ? context->monster.lock() : nullptr;
  const EntityPtr target = context ? context->target.lock() : nullptr;
  if (!context || !context->world || !monster ||
      !IsUsableTarget(context, target, context->config.aggro_range)) {
    return BT::NodeStatus::FAILURE;
  }
  if (IsUsableTarget(context, target, context->config.attack_range)) {
    return BT::NodeStatus::SUCCESS;
  }

  const bool accepted =
      context->world->Move().RequestMoveTo(monster, target->GetPosition());
  return accepted ? BT::NodeStatus::RUNNING : BT::NodeStatus::FAILURE;
}

BT::NodeStatus ChaseTarget::onRunning() {
  const auto context = ContextOf(*this);
  const EntityPtr monster = context ? context->monster.lock() : nullptr;
  const EntityPtr target = context ? context->target.lock() : nullptr;
  if (!context || !context->world || !monster ||
      !IsUsableTarget(context, target, context->config.aggro_range)) {
    CancelMonsterMove(context);
    return BT::NodeStatus::FAILURE;
  }
  if (IsUsableTarget(context, target, context->config.attack_range)) {
    CancelMonsterMove(context);
    return BT::NodeStatus::SUCCESS;
  }
  return context->world->Move().IsMoving(monster)
             ? BT::NodeStatus::RUNNING
             : BT::NodeStatus::FAILURE;
}

void ChaseTarget::onHalted() {
  CancelMonsterMove(ContextOf(*this));
}

AttackTarget::AttackTarget(const std::string& name,
                           const BT::NodeConfig& config)
    : BT::SyncActionNode(name, config) {}

BT::NodeStatus AttackTarget::tick() {
  const auto context = ContextOf(*this);
  const EntityPtr monster = context ? context->monster.lock() : nullptr;
  const EntityPtr target = context ? context->target.lock() : nullptr;
  if (!context || !monster ||
      !IsUsableTarget(context, target, context->config.attack_range)) {
    return BT::NodeStatus::FAILURE;
  }

  CancelMonsterMove(context);
  if (!context->attack_handler || context->attack_handler(monster, target)) {
    return BT::NodeStatus::SUCCESS;
  }
  return BT::NodeStatus::FAILURE;
}

Idle::Idle(const std::string& name, const BT::NodeConfig& config)
    : BT::SyncActionNode(name, config) {}

BT::NodeStatus Idle::tick() {
  return BT::NodeStatus::SUCCESS;
}

void RegisterMonsterAiNodes(BT::BehaviorTreeFactory& factory) {
  factory.registerNodeType<AcquireTarget>("AcquireTarget");
  factory.registerNodeType<IsTargetInAttackRange>("IsTargetInAttackRange");
  factory.registerNodeType<ChaseTarget>("ChaseTarget");
  factory.registerNodeType<AttackTarget>("AttackTarget");
  factory.registerNodeType<Idle>("Idle");
}

}  // namespace game::ai
