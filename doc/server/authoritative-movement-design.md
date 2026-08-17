# 3D 动作 RPG 生产级权威移动方案

> 状态：目标设计，尚未完成代码落地  
> 适用工程：`src/gameserver/` + Unity 客户端  
> 基线：服务端 Jolt Physics 5.6.1、30Hz World Tick、AOI 增量广播  
> 最后更新：2026-08-17

## 1. 最终决策

本项目采用以下生产目标：

> 客户端上传移动输入，服务端在固定 tick 中通过 Jolt 角色控制器计算权威位置；本地玩家立即预测并按服务端状态校正，远端玩家消费服务端快照并插值显示。

明确边界：

- 客户端不提交可信的 `x/y/z` 和速度；可以附带预测位置用于诊断，但服务端不得据此推进角色。
- 速度、加速度、跳跃冲量、闪避距离、硬直和击退全部来自服务端配置与权威状态机。
- 普通有效移动输入不做逐包传统 `req/res`；权威快照中的 `last_processed_input_seq` 同时承担累计 ACK。
- 协议错误、非法状态、限流和断线等拒绝可以立即回复；位置结果必须在 tick 模拟完成后产生。
- 服务器位置是逻辑真值；本地预测位置只是视觉和操作响应层，不参与服务端伤害、AOI、持久化或反作弊判定。
- 普通玩家地面移动、空中控制、跳跃、闪避、击退与技能位移统一进入同一权威移动状态机，禁止多个 Handler 直接写坐标。
- 服务端玩家控制器优先采用 Jolt `CharacterVirtual`，不继续使用“Transform 目标位置驱动普通 Kinematic Body”作为最终玩家移动方案。

该方案适合龙之谷风格的副本动作、Boss 战和 PvP。它在操作响应、反作弊和多人一致性之间取得可控平衡。

## 2. 为什么不会因为服务器 tick 而卡

本地玩家不等待一次网络往返才显示移动：

1. Unity 收到摇杆或键盘输入后立即运行本地预测。
2. 同一输入带序号发送给服务器。
3. 服务端在下一个固定 tick 中计算权威结果。
4. 客户端收到结果后，以已确认输入序号为基线重放尚未确认的输入。
5. 小误差平滑消除，大误差快速纠正，传送等离散事件直接重置。

在 30Hz 下，单个逻辑帧为 33.33ms。若客户端完全等待服务器且 RTT 为 80ms，平均可见输入延迟约为 `80ms + 16.67ms`，动作手感会明显变差；客户端预测将本地视觉响应降到当前渲染帧内。

远端玩家没有当前客户端可用的实时输入，因此使用服务器快照插值。远端显示会故意落后服务器一小段时间，以交换稳定和抗抖动能力；这不影响本地操作响应。

## 3. 当前实现与目标实现的差距

### 3.1 当前代码

- `protobuf/protos/client_common.proto` 的 `entity_move_data` 同时承载请求、响应和 AOI 通知，并包含客户端提交的 `pos`、`velocity`。
- `handlers/move_handler.cpp` 收到 `cli_3d_move_req` 后直接校验客户端绝对位置，再调用 `SetMoveState` 和 `WorldSystem::MoveEntity`。
- `handlers/jump_handler.cpp` 同样采用客户端提交的空中位置和速度。
- `JoltSystem::SyncAllBodies` 从 ECS Transform 读取目标中心，再调用 `MoveKinematicBody`；数据方向是 ECS → Jolt。
- `WorldSystem::Tick` 当前依次推进怪物 AI、NPC MoveSystem、同步运动学刚体、Jolt Update、AOI FlushDirty。
- 网络收包、Handler 与 World Tick 在同一个 EventLoop 线程执行；Mongo 完成回调才切回主循环。
- 移动通道当前是可靠有序 TCP。

### 3.2 目标差距

| 当前 | 目标 |
|------|------|
| 客户端上传绝对坐标 | 客户端上传输入意图 |
| Handler 立即修改 Transform | Handler 只写输入缓冲 |
| Transform 驱动 Kinematic Body | CharacterVirtual 计算后回写 Transform |
| 每个 move 请求立即返回位置 | tick 快照累计确认输入 |
| 本地和远端都消费位置包 | 本地预测/校正，远端插值 |
| TCP 承载全部实时数据 | 登录业务保留 TCP，生产移动优先不可靠时序通道 |
| 跳跃/闪避各自直接改位置 | 统一权威移动状态机 |

本文描述的是目标架构。现有 `flows-move.md`、`move-system.md` 仍描述当前已实现行为，在迁移完成前不要把目标设计当作运行事实。

## 4. 角色和数据所有权

### 4.1 服务器

服务器对以下数据拥有最终权威：

- 位置、旋转、线速度、接地状态和所处移动模式；
- 最大速度、加速度、摩擦、重力、坡度和台阶能力；
- 跳跃次数、闪避次数、耐力、冷却、硬直、击退和技能 Root Motion；
- 地图碰撞、动态障碍、传送、复活和掉线重连基线；
- AOI 所见位置、战斗判定位置和持久化位置。

### 4.2 本地玩家

在某一客户端上，由该客户端直接操作的角色称为本地玩家。客户端拥有输入，但不拥有最终状态：

- 立即预测自身移动；
- 保存未确认输入；
- 消费 owner 快照，校正并重放；
- 摄像机跟随预测胶囊或独立的平滑视觉节点，不能直接跟随瞬时纠错后的网络节点。

### 4.3 远端玩家

同一客户端上由其他用户控制的角色称为远端玩家：

- 不重演其原始输入；
- 保存服务器快照；
- 在延迟渲染时间线上插值；
- 只在短时间缺包时有限外推；
- 传送、复活、跨场景和强制抓取等事件禁止插值穿越。

服务器本身不区分“本地玩家”和“远端玩家”；这只是客户端表现策略。

## 5. 端到端时序

### 5.1 普通移动

```text
Unity Input
  ├─ 本地预测：立即更新本地角色表现
  └─ MoveInput(seq, client_tick, direction, buttons, facing)
          ↓
GameServer EventLoop / MoveInputHandler
  ├─ 会话、序号、频率、参数和状态前置校验
  └─ 写入 PlayerMoveInputComponent，不修改 Transform
          ↓
World Tick 30Hz
  ├─ 消费截至本 tick 的输入
  ├─ 权威移动状态机决定期望速度/冲量
  ├─ CharacterVirtual::ExtendedUpdate
  ├─ 读取权威脚底坐标并调用 WorldSystem::MoveEntity
  ├─ 记录历史状态，标记复制和持久化脏位
  └─ 复制阶段
       ├─ OwnerMoveSnapshot(last_processed_input_seq, state)
       └─ AOI RemoteMoveSnapshot(state)
```

### 5.2 本地校正

```text
收到 owner snapshot
  → 丢弃 seq <= last_processed_input_seq 的历史输入
  → 保存当前视觉偏移
  → 以服务器权威状态重置预测模拟
  → 按序重放尚未确认输入
  → 将视觉偏移在短时间内衰减到 0
```

### 5.3 远端插值

```text
snapshot N(pos A, vel A, tick N)
snapshot N+1(pos B, vel B, tick N+1)
  → 插入按 server_tick 排序的快照缓冲
  → render_time = estimated_server_time - interpolation_delay
  → 在 A/B 之间插值位置，Slerp 旋转
```

## 6. 固定 tick 和处理顺序

### 6.1 基准频率

第一阶段采用以下默认值，必须配置化并通过压测调整：

| 项目 | 默认值 | 说明 |
|------|--------|------|
| 权威世界/角色模拟 | 30Hz | `fixed_dt = 1/30s`，保持现有服务端预算 |
| 客户端输入发送 | 上限 30Hz | 输入变化立即发送，静止时低频 keepalive |
| owner 权威快照 | 20～30Hz | 有移动、纠错或状态切换时发送 |
| 近景远端快照 | 20Hz | AOI 内合并同 tick 多次变化 |
| 远景快照 | 5～10Hz | 复用现有 AOI detail level 做 LOD |
| 远端插值延迟 | 2～3 个快照间隔 | 根据抖动动态调整 |
| 最大外推时间 | 100～150ms | 超时后减速并冻结，等待新快照 |

物理 tick 和网络快照频率必须解耦；不要求每个 tick 都向每个观察者发包。

### 6.2 tick 流水线

目标顺序：

```cpp
void WorldSystem::TickFixed(float fixed_dt) {
    ++server_tick_;

    DrainPlayerInputs(server_tick_);
    ApplyGameplayForces(server_tick_);       // 击退、硬直、技能位移
    monster_ai_system_.Tick(fixed_dt);
    full_move_system_.Tick(fixed_dt);        // NPC

    player_character_system_.StepCharacters(fixed_dt); // SetLinearVelocity + ExtendedUpdate
    jolt_server_->Update(fixed_dt, collision_steps);    // 动态刚体随后推进
    player_character_system_.CommitToWorld(server_tick_);

    movement_history_.Record(server_tick_);
    replication_system_.BuildSnapshots(server_tick_);
    aoi_.FlushDirty();
}
```

`CharacterVirtual` 不由 `PhysicsSystem::Update` 自动推进，需要应用层显式调用 `Update/ExtendedUpdate`。Jolt 官方示例在 `PrePhysicsUpdate` 中推进 CharacterVirtual，因此本方案默认先推进角色、再执行 `PhysicsSystem::Update`；移动平台、落体和推箱仍必须用集成测试确认。最终顺序只能存在一份，不允许不同系统自行调用不同 dt。

### 6.3 timer 抖动和追帧

生产实现不得直接把一次定时器实际间隔作为任意 `dt` 传给角色模拟。使用单调时钟和 accumulator：

```text
accumulator += clamp(real_elapsed, 0, max_frame_elapsed)
while accumulator >= fixed_dt and steps < max_catchup_steps:
    TickFixed(fixed_dt)
    accumulator -= fixed_dt
```

建议 `max_catchup_steps = 3～4`。超过后记录 overload 指标并丢弃过量积压，禁止无限追帧导致雪崩。P99 tick 耗时必须低于 33.33ms，常态目标应显著低于预算。

## 7. 服务端模块设计

### 7.1 新增组件

建议新增：

```text
ecs/components/player_move_input_component.*
  - 输入环形队列
  - last_received_input_seq
  - last_processed_input_seq
  - last_input_arrival_time
  - session_epoch

ecs/components/character_motor_component.*
  - movement_mode
  - authoritative velocity
  - grounded / ground_normal
  - jump/dodge/root-motion 状态
  - CharacterVirtual 句柄或 JoltSystem entry key
```

输入环形队列建议容量 64～128。容量满时优先丢弃已过期的旧输入；持续溢出需要限流并记录异常。

### 7.2 新增系统

建议新增 `PlayerMovementSystem`：

- 消费玩家输入；
- 执行移动状态机；
- 从服务端属性表取得速度和动作参数；
- 驱动 Jolt CharacterVirtual；
- 将结果提交给 `WorldSystem::MoveEntity`；
- 生成 owner/AOI 复制脏状态；
- 保存战斗回溯历史。

`MoveSystem` 继续负责 NPC 路径移动，避免在第一阶段把玩家预测协议与 NPC 寻路强行合并。二者最终都必须通过 `WorldSystem::MoveEntity` 提交空间结果。

### 7.3 Handler 的最终职责

`MoveInputHandler` 只允许：

- 验证连接、实体、场景和 session epoch；
- 验证包大小、字段有限性、方向范围和输入 bitmask；
- 验证序号窗口、速率和重复包；
- 将输入写入组件；
- 对硬拒绝立即返回错误。

禁止：

- 直接调用 `SetPosition`、`MoveEntity` 或 Jolt 移动 API；
- 采用客户端速度作为最终速度；
- 使用客户端时间直接决定服务端 dt；
- 在 Handler 中执行角色物理步进。

### 7.4 单线程约束

当前网络收包、Handler 和 World Tick 均在 GameServer EventLoop 中，可以无锁写入和消费输入组件。若后续 UDP/QUIC 收包使用独立线程，必须把解析后的不可变输入命令投递到 EventLoop；禁止网络线程直接访问 Entity、World 或 Jolt。

## 8. Jolt 角色控制器

### 8.1 选择

玩家使用 `JPH::CharacterVirtual`，原因是它提供角色移动需要的墙体滑动、坡度、台阶、地面判断和移动平台交互。当前仓库的 Jolt 5.6.1 已包含该实现。

普通 NPC 可以继续沿用 MoveSystem；需要完整物理交互的 Boss/NPC 再按类型迁移，避免一次性扩大范围。

### 8.2 坐标语义

本项目 `TransformComponent::pos_` 是脚底世界坐标；Jolt Character 通常以角色中心位置工作。必须集中定义：

```text
character_center = foot_position + up * (half_height + radius)
foot_position = character_center - up * (half_height + radius)
```

转换只能由 Jolt/Character 适配层实现。Handler、AOI、持久化和战斗系统一律使用脚底坐标，禁止各模块重复加减高度。

### 8.3 服务端运动参数

客户端只提供单位方向和模拟摇杆幅度，以下值由服务器决定：

- walk/run 最大速度；
- 地面/空中加速度与减速度；
- 重力和终端速度；
- 跳跃初速度与二段跳次数；
- 最大坡度、台阶高度、贴地距离；
- 闪避、击退和 Root Motion 曲线；
- 不同职业、Buff、Debuff 和装备对参数的修正。

方向长度必须夹到 `[0, 1]`。服务器根据最终状态计算 `desired_velocity`，再设置 Character 线速度；绝不能直接使用客户端上报的速度标量。

### 8.4 玩家之间的碰撞

默认生产策略：

- PvE 同队玩家不做刚性互堵，避免门口拥塞和网络纠错放大；
- 玩家与世界静态碰撞、移动平台和必要动态障碍发生碰撞；
- 玩家受击体由权威 Transform/历史胶囊提供，不依赖玩家之间的刚体互撞；
- 若 PvP 需要角色碰撞，单独开启并进行密集人群压力测试，不与基础版本绑定发布。

### 8.5 特殊移动

服务器状态机建议至少包含：

```text
Grounded
Airborne
Dodge
RootMotion
Knockback
Stunned
Dead
Teleport
```

状态优先级由服务端决定。例如 `Dead/Stunned/Knockback` 必须能覆盖普通移动输入。跳跃、闪避和技能位移是“动作意图”，不再允许客户端提交动作终点。

## 9. 协议设计

### 9.1 不复用旧 entity_move_data 作为输入

旧 `entity_move_data` 同时用于 req/res/AOI，语义混合。新增 V2 消息，保留旧协议用于灰度和回滚。

示例：

```protobuf
message move_input_command {
    uint32 input_seq       = 1;
    uint32 client_tick     = 2;
    sint32 move_x_q15      = 3; // [-32767, 32767]
    sint32 move_z_q15      = 4;
    uint32 facing_yaw_q16  = 5;
    uint32 buttons         = 6;
}

message cli_3d_move_input_req {
    uint32 session_epoch = 1;
    repeated move_input_command commands = 2; // 当前命令 + 最近若干命令冗余
}

message authoritative_move_state {
    uint64 entity_id               = 1;
    uint32 server_tick             = 2;
    uint32 last_processed_input_seq = 3;
    vec3 position                  = 4;
    vec3 velocity                  = 5;
    quat rotation                  = 6;
    move_status status             = 7;
    uint32 state_flags             = 8;
    uint32 teleport_id             = 9;
}

message cli_3d_move_owner_ntf {
    authoritative_move_state state = 1;
    error_code correction_reason   = 2;
}

message cli_3d_move_reject_ntf {
    uint32 input_seq = 1;
    error_code err_code = 2;
    authoritative_move_state state = 3;
}
```

AOI 更新可以继续复用 `cli_3d_aoi_update_ntf` 外壳，但其中 MOVE_DATA 应升级为包含 `server_tick`、权威状态和 teleport 标志的新载荷。不要让远端客户端依赖 owner 专属 ACK。

### 9.2 序号规则

- `input_seq` 使用 `uint32`，按模回绕比较；禁止直接用 `<` 处理回绕。
- 同一 session epoch 内严格递增；重复输入幂等丢弃。
- 重连、重新进图和传送重置预测基线并增加 `session_epoch`，旧连接的迟到包全部失效。
- 服务端每 tick 至多处理配置的最大输入数，避免恶意客户端用历史输入制造追帧。
- 没有新输入时可短暂保持最后方向；超过 100～150ms 后强制中性输入，防断网继续行走。

### 9.3 时间字段

- `server_tick` 是权威时间线。
- `client_tick` 用于预测队列、诊断和时钟映射，不直接作为服务端物理 dt。
- 通过 ping/pong 维护客户端对服务器 tick 的估计。
- 服务端接受的输入时间窗口必须有限，过旧或过未来输入丢弃并计数。

## 10. 网络传输

### 10.1 最终生产形态

- TCP：登录、角色、背包、任务、聊天、场景切换、可靠技能事件和持久化业务。
- 不可靠时序通道：高频移动输入和移动快照；可选 UDP 或支持 Datagram 的传输层。
- 每个输入包冗余携带最近 2～3 条未确认命令；每个权威快照可覆盖之前状态，因此允许丢弃过期快照。

可靠有序 TCP 在丢包时会产生队头阻塞；旧移动包补传成功前，新位置也会被阻塞。当前 TCP 可以用于第一阶段功能验证，但生产发布前必须在真实弱网条件下验证；若达不到抖动目标，应完成实时通道拆分。

### 10.2 发包规则

- 不把网络发包次数等同于物理 tick 次数。
- 同一实体一个复制周期只保留最新移动状态。
- owner 与远端快照可以共享序列化数据，但路由和消费逻辑不同；即使位置未改变，只要累计 ACK 前进，也必须按 owner 发送节奏确认输入。
- enter/leave/teleport/dead 等离散状态走可靠事件，并带新的基线 ID。
- 快照按 AOI detail level、距离、战斗相关性和带宽预算降频。

## 11. Unity 客户端

### 11.1 本地预测

客户端维护：

- `next_input_seq`；
- 未确认输入环形队列；
- 预测逻辑胶囊；
- 权威基线状态；
- 视觉平滑偏移。

Unity 客户端可以使用 CharacterController 或自定义胶囊 sweep 预测，不要求与 Jolt 跨平台位级确定。必须对齐胶囊尺寸、速度、加速度、重力、坡度、台阶和移动模式；差异由校正系统收敛。

若墙角、台阶和移动平台持续产生高频纠错，再评估共享 C++ movement motor 或在 Unity 集成 Jolt 原生插件，而不是一开始就要求跨引擎完全确定。

### 11.2 校正策略

初始建议值，必须由实机统计调整：

| 误差 | 处理 |
|------|------|
| `< 3cm` | 忽略或极慢收敛 |
| `3～50cm` | 80～150ms 视觉平滑 |
| `50～150cm` | 快速平滑并记录原因 |
| `> 150cm` | 硬纠正；传送标志则立即重置 |

逻辑碰撞体应在重放后位于预测结果；渲染模型使用独立偏移平滑，避免摄像机和特效随每次小纠错抖动。

### 11.3 远端插值

- 按 `server_tick` 存储至少 250～500ms 快照历史；
- 插值延迟根据快照间隔和到达抖动动态取 2～3 个间隔；
- 位置可采用线性或 Hermite 插值，旋转使用 Slerp；
- 缺包时按服务器速度外推不超过 100～150ms；
- 超时后减速/冻结，不无限外推；
- teleport、复活、跨图、抓取和强制位移按离散事件处理。

### 11.4 动画

移动动画不能反过来决定普通权威位移：

- 普通 locomotion 从权威/预测速度派生动画参数；
- 跳跃、闪避、击退和技能由权威状态机触发；
- Root Motion 技能使用服务端可执行的位移曲线或运动参数；
- 客户端动画事件只负责表现，伤害窗口由服务端技能时间线决定。

## 12. AOI、战斗与持久化

### 12.1 AOI

`WorldSystem::MoveEntity` 继续作为位置提交的唯一入口：

- 更新 Transform；
- 更新 Map 空间索引；
- 更新 AOI subject/watcher；
- 标记复制脏位；
- 跨格触发对应事件。

区别只是调用来源由 Handler 变成 `PlayerMovementSystem::CommitToWorld`。同一个 tick 内多次子步只在 tick 末广播最终状态，避免包风暴。

### 12.2 战斗

服务端保存至少最近 0.5～1 秒的移动历史：

```text
(server_tick, position, rotation, capsule, velocity, movement_mode)
```

命中检测只能使用服务器历史状态。若启用延迟补偿，回溯窗口必须配置上限并记录使用量；客户端提交的攻击位置只能作为请求信息，不得覆盖历史位置。

闪避无敌帧、受击硬直、击退和技能位移要与移动状态机共享 server tick，禁止各自使用独立墙钟计时。

### 12.3 持久化

Mongo 只保存服务器确认后的 Transform：

- 不保存客户端预测位置；
- 不在每个 tick 落库；
- 继续使用现有 dirty + 定时异步持久化；
- 下线、踢人、切图按现有强制落地路径保存最后权威位置。

## 13. 安全与反作弊

服务端至少实施：

- 单连接和单实体移动包速率限制；
- 输入序号、session epoch、过期窗口和未来窗口检查；
- NaN/Inf、方向长度、非法 bitmask 和非法状态转换检查；
- 速度、加速度、跳跃、闪避和技能参数全部服务端生成；
- 输入超时自动归零；
- 传送、复活和跨图只能由服务端事件触发；
- 连续纠错、输入洪泛、非法状态和时间漂移形成风险评分；
- 风险评分用于观测、限流和人工规则，不因一次网络抖动直接封禁。

客户端预测坐标可以作为 telemetry 上传，用于测量误差和发现异常，但永远不能成为权威位置。

## 14. 可观测性和运行指标

### 14.1 服务端指标

- `world_tick_duration_ms`：P50/P95/P99/max；
- `world_tick_catchup_steps`、`world_tick_dropped_time_ms`；
- `move_inputs_received/dropped/duplicate/expired/future/rate_limited`；
- `move_input_queue_depth`、`move_input_age_ms`；
- `character_update_duration_ms`、活动 Character 数量；
- owner/AOI 快照数量、字节数、按 detail level 的降频量；
- 纠错距离直方图和纠错原因；
- teleport、stuck、ground-state flip 和碰撞失败计数。

### 14.2 客户端指标

- RTT、抖动、丢包、快照间隔；
- 未确认输入队列长度；
- 本地预测误差 P50/P95/P99；
- 软纠正和硬纠正次数；
- 远端插值缓冲深度、外推时间和冻结次数；
- 客户端帧率与预测 tick 追帧次数。

高频日志必须采样或聚合，禁止每玩家每 tick 打 INFO。

## 15. 测试与生产验收

### 15.1 单元测试

- 输入序号回绕、重复、乱序、过旧和未来输入；
- 输入超时归零；
- 走/跑、加减速、转向、坡度、台阶、墙体滑动；
- 起跳、二段跳、下落、落地、闪避、击退和状态抢占；
- 脚底坐标与 Jolt 中心坐标转换；
- teleport/reconnect 后旧输入失效；
- 同一服务端构建对同一输入录制进行确定性重放。

### 15.2 集成测试

- Handler 收包后 Transform 不立即变化，只在 tick 后变化；
- tick 后 owner 快照正确累计 ACK；
- AOI 观察者收到权威状态，自己不重复消费远端路径；
- 同格/跨格移动、appear/disappear 和 watcher 重建；
- 下线、顶号、重连、切图和持久化位置一致；
- Jolt 静态地图、移动平台、动态障碍和角色接触。

### 15.3 弱网矩阵

至少覆盖：

| 维度 | 测试值 |
|------|--------|
| RTT | 0 / 50 / 100 / 200 / 300ms |
| 抖动 | 0 / 10 / 30 / 60ms |
| 丢包 | 0 / 1 / 3 / 5 / 10% |
| 乱序/重复 | 0 / 1 / 3% |
| 客户端 FPS | 20 / 30 / 60 / 120 |
| 服务端卡顿注入 | 50 / 100 / 250ms |

### 15.4 初始发布门槛

- 30Hz 下 P99 World Tick 不超过 33.33ms，且不会无限追帧；
- 正常网络、平地连续移动不出现肉眼可见周期性拉扯；
- 100ms RTT、3% 丢包下，本地仍即时响应，远端无持续瞬移；
- 非 teleport 场景硬纠正率低于约定阈值，并有原因分布；
- 伪造位置、速度、旧 session、输入洪泛不能改变服务端权威状态；
- 断线重连后位置、输入序号、AOI 和持久化基线一致；
- 24 小时 soak 无输入队列、Character、AOI dirty 或快照缓冲泄漏。

具体数值门槛应由首轮实机基线固化，不能仅以“手感正常”验收。

## 16. 分阶段实施

### Phase 0：协议和测量基线

- 新增 V2 输入/owner snapshot 协议，不破坏旧消息号；
- 加入 server tick、input seq、session epoch 和移动指标；
- 建立弱网模拟与输入录制/重放工具；
- 记录现有客户端坐标方案的延迟、纠错和服务器成本。

退出条件：协议生成链可用，现有测试全通过，基线指标可观测。

### Phase 1：输入缓冲和影子模拟

- Handler 写输入组件，不影响线上旧位置；
- 新 PlayerMovementSystem 在 shadow 模式计算位置；
- 对比 shadow 位置与旧路径位置，不向客户端生效；
- 完成序号、限流、超时和重连重置。

退出条件：平地、坡道、台阶和墙角误差有统计，系统可按开关关闭。

### Phase 2：Jolt CharacterVirtual 权威落地

- 玩家从普通 Kinematic Body 迁移到 CharacterVirtual；
- tick 计算后通过 `MoveEntity` 提交权威位置；
- 旧 `cli_3d_move_req` 仅用于兼容灰度账号；
- Mongo、AOI 和战斗统一读取权威 Transform。

退出条件：服务端单机、集成、AOI 和移动压力测试通过。

### Phase 3：Unity 预测、校正和远端插值

- 本地输入缓存、预测、累计 ACK、重放和视觉平滑；
- 远端快照缓冲、插值、有限外推和 teleport 重置；
- 加入延迟、抖动、丢包和不同 FPS 自动化测试。

退出条件：100ms RTT/3% 丢包目标满足，硬纠正原因可解释。

### Phase 4：空中、闪避和战斗位移

- 将 jump/dodge 从客户端位置上报改为动作意图；
- 服务端状态机处理重力、空中控制、Root Motion、击退和硬直；
- 接入移动历史和服务端命中判定。

退出条件：动作状态不存在双重驱动，技能/移动/动画时间线一致。

### Phase 5：实时传输和规模化

- 在 TCP 之外接入不可靠时序移动通道；
- 输入冗余、快照覆盖、AOI LOD、量化和带宽预算；
- 灰度、回滚、告警、容量和 24 小时 soak。

退出条件：生产网络门槛、容量门槛和回滚演练全部通过。

## 17. 灰度与回滚

- 以账号、地图实例或服务器进程为粒度开启 `authoritative_movement_v2`；
- 同一地图实例禁止 V1/V2 玩家使用不同碰撞真值互相战斗；灰度期间需要隔离实例或明确兼容层；
- 所有新消息使用独立协议号和能力协商；
- 服务端保留 V1 只作为迁移期回滚，不作为长期双轨；
- 回滚时必须发送新的 teleport/baseline ID，清空客户端预测和远端快照缓存；
- 数据库结构无需因预测层改变，仍持久化权威 Transform。

## 18. 代码改造地图

| 文件/模块 | 改造 |
|-----------|------|
| `protobuf/protos/client_common.proto` | 新增 input/state V2 数据，不复用旧混合结构 |
| `protobuf/protos/client_3d.proto` | 新增 move input、owner snapshot、reject 消息 |
| `handlers/move_handler.*` | 改为输入接收与拒绝，不写位置 |
| `handlers/jump_handler.*` | 迁移为动作意图，最终并入移动状态机 |
| `ecs/components/transform_component.h` | 保留权威输出，补 server tick/模式可放新组件 |
| `ecs/components/player_move_input_component.*` | 新增输入队列和 ACK 状态 |
| `ecs/components/character_motor_component.*` | 新增角色控制器状态 |
| `ecs/systems/player_movement_system.*` | 新增玩家权威移动系统 |
| `ecs/systems/jolt_system.*` | 管理 CharacterVirtual，移除玩家 Transform→Body 主路径 |
| `ecs/systems/world_system.*` | 固定 tick 顺序、server tick、提交和历史记录 |
| `ecs/entity/player_entity.cpp` | 序列化新的权威移动载荷 |
| `ecs/systems/aoi_*` | 复制频率、合并和 detail level LOD |
| `game_server.*` | fixed-step accumulator、能力协商、指标和实时通道路由 |
| `src/client/` | 扩展测试客户端，验证序号、ACK、弱网和重连 |

## 19. 不采用的方案

### 客户端绝对位置权威

优点是实现简单、手感即时；缺点是瞬移、加速、穿墙和多人状态分歧难以根治。不作为最终方案。

### 客户端完全等待服务器位置

实现简单但输入延迟约等于 RTT 加 tick 等待，不适合动作 RPG。不采用。

### 向所有客户端转发所有人的原始输入

要求所有客户端重演完整世界、处理确定性和作弊边界，且 AOI 大场景带宽和 CPU 成本高。不采用。远端只消费权威快照。

### 首期强求 Unity 与 Jolt 位级确定

跨引擎、跨平台浮点和碰撞细节成本过高。采用状态校正和重放收敛；只有实测证明墙角等场景不可接受时，再共享 movement motor。

## 20. 外部参考

- [Unreal Engine：Networked Character Movement](https://dev.epicgames.com/documentation/unreal-engine/understanding-networked-movement-in-the-character-movement-component-for-unreal-engine?lang=en-US)：本地预测、SavedMove、服务器重演、ACK/纠正和远端平滑。
- [Valve：Source Multiplayer Networking](https://developer.valvesoftware.com/wiki/Source_Multiplayer_Networking)：服务器 tick、快照、客户端预测、插值和延迟补偿。
- [Rocket League GDC：It IS Rocket Science](https://media.gdcvault.com/gdc2018/presentations/Cone_Jared_It_Is_Rocket.pdf)：服务端缓存输入、客户端预测网络物理。
- [Overwatch GDC：Gameplay Architecture and Netcode](https://gdcvault.com/play/1024001/-Overwatch-Gameplay-Architecture-and)：响应性、精度与网络模拟设计。
- [Jolt：Character Controller Architecture](https://github.com/jrouwe/JoltPhysics/blob/master/Docs/Architecture.md#character-controllers)：Character 与 CharacterVirtual 的用途和更新模型。
- [Jolt：CharacterVirtual API](https://jrouwe.github.io/JoltPhysicsDocs/5.2.0/class_character_virtual.html)：速度、Update/ExtendedUpdate、坡度、台阶和接触接口。

## 21. 一句话实施原则

> 输入可以预测，表现可以平滑，网络可以丢包；权威位置只能由服务端固定 tick 中的移动状态机和 Jolt 角色控制器产生。
