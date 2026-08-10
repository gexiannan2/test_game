#include "ai/behavior_tree/monster_ai_system.h"

#include <algorithm>
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

class MonsterAiSystem::Impl {
 public:
  struct Runtime {
    Runtime(std::shared_ptr<MonsterAiContext> value_context,
            BT::Tree value_tree)
        : context(std::move(value_context)), tree(std::move(value_tree)) {}

    std::shared_ptr<MonsterAiContext> context;
    BT::Tree tree;
    float tick_accumulator = 0.0f;
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
  normalized.aggro_range = std::max(0.0f, normalized.aggro_range);
  normalized.attack_range =
      std::clamp(normalized.attack_range, 0.0f, normalized.aggro_range);
  normalized.move_speed = std::max(0.0f, normalized.move_speed);
  normalized.tick_interval = std::max(0.01f, normalized.tick_interval);

  MoveComponent* move = monster->GetComponent<MoveComponent>();
  if (!move) {
    move = &monster->AddComponent<MoveComponent>();
  }
  move->SetSpeed(normalized.move_speed);

  DetachMonster(monster->GetId());

  auto context = std::make_shared<MonsterAiContext>();
  context->world = impl_->world;
  context->monster = monster;
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

void MonsterAiSystem::Tick(float dt) {
  if (!impl_->world || dt <= 0.0f || impl_->runtimes.empty()) {
    return;
  }

  std::vector<uint64_t> entity_ids;
  entity_ids.reserve(impl_->runtimes.size());
  for (const auto& [entity_id, runtime] : impl_->runtimes) {
    (void)runtime;
    entity_ids.push_back(entity_id);
  }

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

    runtime.tick_accumulator += dt;
    if (runtime.tick_accumulator < runtime.context->config.tick_interval) {
      continue;
    }
    runtime.tick_accumulator = 0.0f;
    runtime.tree.tickOnce();
  }
}

}  // namespace game::ai
