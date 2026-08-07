#include "ai/behavior_tree/monster_ai_system.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include <behaviortree_cpp/bt_factory.h>

#include "ai/behavior_tree/monster_ai_context.h"
#include "ai/behavior_tree/monster_ai_nodes.h"
#include "basic_monster_tree_xml.h"
#include "ecs/components/move_component.h"
#include "ecs/entity/entity.h"
#include "ecs/systems/move_system.h"
#include "ecs/systems/world_system.h"

namespace game::ai {
namespace {

float NonNegativeOr(float value, float fallback) {
  return std::isfinite(value) ? std::max(0.0f, value) : fallback;
}

}  // namespace

class MonsterAiSystem::Impl {
 public:
  struct Runtime {
    Runtime(std::shared_ptr<MonsterAiContext> value_context,
            BT::Tree value_tree)
        : context(std::move(value_context)), tree(std::move(value_tree)) {}

    std::shared_ptr<MonsterAiContext> context;
    BT::Tree tree;
    double tick_accumulator = 0.0;
  };

  Impl() { RegisterMonsterAiNodes(factory); }

  WorldSystem* world = nullptr;
  TargetSelector target_selector;
  AttackHandler attack_handler;
  BT::BehaviorTreeFactory factory;
  std::unordered_map<uint64_t, std::unique_ptr<Runtime>> runtimes;
};

MonsterAiSystem::MonsterAiSystem() : impl_(std::make_unique<Impl>()) {}

MonsterAiSystem::~MonsterAiSystem() = default;

void MonsterAiSystem::Bind(WorldSystem* world) {
  impl_->world = world;
}

void MonsterAiSystem::SetTargetSelector(TargetSelector selector) {
  impl_->target_selector = std::move(selector);
  for (auto& [entity_id, runtime] : impl_->runtimes) {
    (void)entity_id;
    runtime->context->target_selector = impl_->target_selector;
  }
}

void MonsterAiSystem::SetAttackHandler(AttackHandler handler) {
  impl_->attack_handler = std::move(handler);
  for (auto& [entity_id, runtime] : impl_->runtimes) {
    (void)entity_id;
    runtime->context->attack_handler = impl_->attack_handler;
  }
}

bool MonsterAiSystem::AttachMonster(const EntityPtr& monster,
                                    const MonsterAiConfig& config) {
  if (!impl_->world || !monster || !monster->IsInMap() ||
      monster->GetEntityType() != EntityType::kMarch) {
    return false;
  }

  MonsterAiConfig normalized = config;
  const MonsterAiConfig defaults;
  normalized.aggro_range =
      NonNegativeOr(normalized.aggro_range, defaults.aggro_range);
  normalized.attack_range =
      std::clamp(NonNegativeOr(normalized.attack_range,
                               defaults.attack_range),
                 0.0f, normalized.aggro_range);
  normalized.leash_range =
      std::max(0.01f,
               NonNegativeOr(normalized.leash_range, defaults.leash_range));
  normalized.return_tolerance =
      std::clamp(NonNegativeOr(normalized.return_tolerance,
                               defaults.return_tolerance),
                 0.0f, normalized.leash_range);
  normalized.move_speed =
      NonNegativeOr(normalized.move_speed, defaults.move_speed);
  normalized.chase_repath_distance =
      std::max(0.01f, NonNegativeOr(normalized.chase_repath_distance,
                                    defaults.chase_repath_distance));
  normalized.tick_interval =
      std::max(0.01f, NonNegativeOr(normalized.tick_interval,
                                    defaults.tick_interval));
  normalized.attack_windup =
      NonNegativeOr(normalized.attack_windup, defaults.attack_windup);
  normalized.attack_cooldown =
      NonNegativeOr(normalized.attack_cooldown, defaults.attack_cooldown);

  MoveComponent* move = monster->GetComponent<MoveComponent>();
  if (!move) {
    move = &monster->AddComponent<MoveComponent>();
  }
  move->SetSpeed(normalized.move_speed);

  DetachMonster(monster->GetId());

  auto context = std::make_shared<MonsterAiContext>();
  context->world = impl_->world;
  context->monster = monster;
  context->home_position = monster->GetPosition();
  context->config = normalized;
  context->target_selector = impl_->target_selector;
  context->attack_handler = impl_->attack_handler;

  auto blackboard = BT::Blackboard::create();
  blackboard->set("monster_context", context);
  BT::Tree tree = impl_->factory.createTreeFromText(kBasicMonsterTreeXml,
                                                    blackboard);
  auto runtime = std::make_unique<Impl::Runtime>(context, std::move(tree));
  runtime->tick_accumulator = normalized.tick_interval;
  impl_->runtimes[monster->GetId()] = std::move(runtime);
  return true;
}

void MonsterAiSystem::DetachMonster(uint64_t entity_id) {
  const auto it = impl_->runtimes.find(entity_id);
  if (it == impl_->runtimes.end()) {
    return;
  }

  const EntityPtr monster = it->second->context->monster.lock();
  it->second->tree.haltTree();
  if (impl_->world && monster) {
    impl_->world->Move().CancelMove(monster, MoveStopReason::kMarchAi);
  }
  impl_->runtimes.erase(it);
}

bool MonsterAiSystem::HasMonster(uint64_t entity_id) const {
  return impl_->runtimes.contains(entity_id);
}

std::size_t MonsterAiSystem::MonsterCount() const {
  return impl_->runtimes.size();
}

std::optional<MonsterAiSnapshot> MonsterAiSystem::InspectMonster(
    uint64_t entity_id) const {
  const auto it = impl_->runtimes.find(entity_id);
  if (it == impl_->runtimes.end()) {
    return std::nullopt;
  }

  const auto& context = it->second->context;
  const EntityPtr target = context->target.lock();
  return MonsterAiSnapshot{
      .state = context->state,
      .target_entity_id = target ? target->GetId() : 0,
      .attack_cooldown_remaining =
          context->attack_cooldown_remaining,
  };
}

void MonsterAiSystem::Tick(float dt) {
  if (!impl_->world || !std::isfinite(dt) || dt <= 0.0f ||
      impl_->runtimes.empty()) {
    return;
  }

  std::vector<uint64_t> entity_ids;
  entity_ids.reserve(impl_->runtimes.size());
  for (const auto& [entity_id, runtime] : impl_->runtimes) {
    (void)runtime;
    entity_ids.push_back(entity_id);
  }
  std::sort(entity_ids.begin(), entity_ids.end());

  for (const uint64_t entity_id : entity_ids) {
    const auto it = impl_->runtimes.find(entity_id);
    if (it == impl_->runtimes.end()) {
      continue;
    }
    Impl::Runtime& runtime = *it->second;
    const EntityPtr monster = runtime.context->monster.lock();
    if (!monster || !monster->IsInMap()) {
      DetachMonster(entity_id);
      continue;
    }

    runtime.context->attack_cooldown_remaining =
        std::max(0.0f,
                 runtime.context->attack_cooldown_remaining - dt);
    if (runtime.context->state == MonsterAiState::kAttackWindup) {
      runtime.context->attack_windup_remaining =
          std::max(0.0f,
                   runtime.context->attack_windup_remaining - dt);
    }

    runtime.tick_accumulator += dt;
    if (runtime.tick_accumulator < runtime.context->config.tick_interval) {
      continue;
    }
    runtime.tick_accumulator =
        std::fmod(runtime.tick_accumulator,
                  static_cast<double>(runtime.context->config.tick_interval));
    try {
      runtime.tree.tickOnce();
    } catch (...) {
      // 单只怪物的节点或业务回调异常不得中断整个场景循环。
      runtime.tree.haltTree();
      runtime.context->target.reset();
      runtime.context->attack_windup_remaining = 0.0f;
      runtime.context->attack_cooldown_remaining = 0.0f;
      runtime.context->state = MonsterAiState::kIdle;
      impl_->world->Move().CancelMove(monster, MoveStopReason::kMarchAi);
    }
  }
}

}  // namespace game::ai
