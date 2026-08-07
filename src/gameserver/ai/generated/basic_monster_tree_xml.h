#pragma once

namespace game::ai {

// 由 basic_monster.xml 在 CMake 配置阶段生成，避免运行目录影响资源加载。
inline constexpr char kBasicMonsterTreeXml[] = R"btree(<root BTCPP_format="4" main_tree_to_execute="BasicMonster">
  <BehaviorTree ID="BasicMonster">
    <Fallback>
      <Sequence>
        <IsTargetInAttackRange/>
        <AttackTarget/>
      </Sequence>
      <Sequence>
        <AcquireTarget/>
        <ChaseTarget/>
      </Sequence>
      <Idle/>
    </Fallback>
  </BehaviorTree>
</root>
)btree";

}  // namespace game::ai
