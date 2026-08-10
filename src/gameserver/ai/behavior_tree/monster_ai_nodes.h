#pragma once

#include <memory>

#include <behaviortree_cpp/action_node.h>
#include <behaviortree_cpp/bt_factory.h>
#include <behaviortree_cpp/condition_node.h>

namespace game::ai {

struct MonsterAiContext;

class AcquireTarget final : public BT::SyncActionNode {
 public:
  AcquireTarget(const std::string& name, const BT::NodeConfig& config);
  static BT::PortsList providedPorts() { return {}; }
  BT::NodeStatus tick() override;
};

class IsTargetInAttackRange final : public BT::ConditionNode {
 public:
  IsTargetInAttackRange(const std::string& name,
                        const BT::NodeConfig& config);
  static BT::PortsList providedPorts() { return {}; }
  BT::NodeStatus tick() override;
};

class ChaseTarget final : public BT::StatefulActionNode {
 public:
  ChaseTarget(const std::string& name, const BT::NodeConfig& config);
  static BT::PortsList providedPorts() { return {}; }

 private:
  BT::NodeStatus onStart() override;
  BT::NodeStatus onRunning() override;
  void onHalted() override;
};

class AttackTarget final : public BT::SyncActionNode {
 public:
  AttackTarget(const std::string& name, const BT::NodeConfig& config);
  static BT::PortsList providedPorts() { return {}; }
  BT::NodeStatus tick() override;
};

class Idle final : public BT::SyncActionNode {
 public:
  Idle(const std::string& name, const BT::NodeConfig& config);
  static BT::PortsList providedPorts() { return {}; }
  BT::NodeStatus tick() override;
};

void RegisterMonsterAiNodes(BT::BehaviorTreeFactory& factory);

}  // namespace game::ai
