# 怪物行为树 AI

## 1. 目录边界

```text
cmake/dependencies/
└── BehaviorTreeCpp.cmake              # 依赖选项与本地源码挂载

third_party/behaviortree_cpp/          # BehaviorTree.CPP 4.10.0 完整源码

src/server/ai/
├── CMakeLists.txt                     # 导出 game_monster_ai 静态库
└── behavior_tree/
    ├── monster_ai_system.*            # 树实例生命周期与低频 Tick
    ├── monster_ai_context.h            # 黑板上下文
    ├── monster_ai_nodes.*              # 索敌、追击、攻击、复位、待机节点
    └── trees/basic_monster.xml         # 默认怪物树的唯一配置源
```

第三方源码不放业务改动；项目适配全部留在 `src/server/ai`。默认 XML 在 CMake
配置阶段嵌入 `game_monster_ai`，因此服务端从任意工作目录启动都不会丢失行为树文件。
XML 已登记到 `CMAKE_CONFIGURE_DEPENDS`，修改树后执行普通增量构建也会重新生成嵌入头。

## 2. 当前行为

默认树按以下优先级执行：

```text
需要脱战复位 → ReturnHome → 到达出生点
       ↓ 否
索敌成功 → 目标在攻击距离 → 前摇 → AttackHandler → 冷却
                 ↓ 否
          ChaseTarget（动态刷新目标点）
       ↓ 否
      Idle
```

- 当前项目尚无 `MonsterEntity`，因此暂用 `EntityType::kMarch` 表示 NPC/怪物。
- 未注入索敌器时，`AcquireTarget` 默认选择警戒范围内最近玩家；等距时选择实体
  ID 最小者，避免哈希容器遍历顺序影响结果。
- `ChaseTarget` 只通过 `MoveSystem::RequestMoveTo` 移动，并在目标位移超过阈值时
  刷新目的地，不直接写坐标。
- 怪物越过以出生点为圆心的 `leash_range` 后清除目标并粘滞返回出生点，返回完成
  前禁止重新索敌。
- `AttackTarget` 是有状态节点，完成可配置前摇后才调用 `AttackHandler`，成功后进入
  冷却；未设置处理器、处理器拒绝或抛异常都不算攻击成功。
- `InspectMonster` 返回纯值状态快照，不暴露 BehaviorTree.CPP 内部类型。
- 黑板只保存 `weak_ptr<Entity>`，离图后不会保活实体。

## 3. 生命周期与 Tick

```text
SpawnOnMap
    ↓
WorldSystem::MonsterAi().AttachMonster
    ↓
WorldSystem::Tick
    ├── MonsterAiSystem::Tick（默认每只怪 5Hz）
    ├── MoveSystem::Tick（世界 30Hz）
    ├── Jolt
    └── AOI Flush
    ↓
LeaveMap → haltTree → CancelMove → DetachMonster
```

AI 在移动之前 Tick，使本帧下发的追击命令能立即推进。每只怪物用独立累加器降低
决策频率，移动与物理仍保持世界帧率。

## 4. 接入示例

```cpp
game::ai::MonsterAiConfig config;
config.aggro_range = 15.0f;
config.attack_range = 2.0f;
config.leash_range = 30.0f;
config.return_tolerance = 0.25f;
config.move_speed = 5.0f;
config.chase_repath_distance = 1.0f;
config.tick_interval = 0.2f;
config.attack_windup = 0.45f;
config.attack_cooldown = 1.0f;

world->MonsterAi().SetAttackHandler(
    [](const EntityPtr& monster, const EntityPtr& target) {
      // 转发到 SkillSystem / CombatSystem。
      return true;
    });

world->MonsterAi().AttachMonster(monster, config);
```

正式战斗接入时，应新增 `MonsterEntity`、怪物属性组件和技能系统；行为树节点只做
决策与系统调用，不负责 protobuf 组包或网络发送。当前前摇和冷却约束的是
“攻击请求”，不等价于技能命中、伤害、硬直或死亡已经完成。详细准入标准见
`monster-ai-test-plan.md`。
