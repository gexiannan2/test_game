# 动态阻挡生产方案（3D 龙之谷世界）

> 结论：**可走性权威在 NavMesh；几何滑墙/贴脸可选挂 Jolt。**  
> 禁止「只放 Jolt、不改 Nav」作为唯一方案——AI 寻路与 Tick 位移校验会分裂。  
> 现状：`NavSystem::add_obstacle*` 已实现；业务未接线；Jolt 无动态阻挡 API。

---

## 1. 问题拆解：动态阻挡到底挡什么？

龙之谷向玩法里，“动态阻挡点”通常同时服务多类消费者：

| 消费者 | 需要什么 | 失败表现 |
|--------|----------|----------|
| 怪物 AI 寻路 | 路径绕开禁行区 | 往墙里钻、卡死重寻 |
| 玩家 Tick 位移 / 移动校验 | 不能穿入禁行体积 | 穿模、作弊穿墙 |
| 技能判定（部分） | 弹道/冲锋是否被挡 | 技能穿墙或莫名弹开 |
| 客户端表现 | 看见墙/圈/门 | 只看得到却走得进（或相反） |
| 物理手感（可选） | 贴墙滑动、不穿透 | 仅逻辑禁行时可能“粘停”无滑步 |

**关键认知：**  
“挡寻路”和“挡刚体接触”是两层问题。用一个系统硬扛两层，要么 AI 错，要么手感差。

---

## 2. NavMesh vs Jolt：深度对比

### 2.1 NavMesh 动态阻挡（本仓库主方案：`setPolyArea`）

机制：障碍 AABB/圆柱覆盖到的 poly → `area=63` + 清 flags → filter 极高代价；删除时恢复。

| 维度 | 评价 |
|------|------|
| AI 绕路 | ✅ 天然生效（findPath / follow） |
| 玩家 `validate_*` / raycast | ✅ 已补 AABB 盲区，可拒非法位移 |
| 与 Tick 权威移动 | ✅ 同步、同线程、&lt;1ms 级 |
| 精度 | ⚠️ poly 级（略偏大，可接受） |
| 工程成熟度 | ✅ API 已在 `navigation_mesh_handle` / `NavSystem` |
| TileCache 重建 | ❌ 未编进 gameserver；文档明确不推荐服务器主路径 |
| 贴墙滑动手感 | ❌ 不提供连续接触响应 |

### 2.2 Jolt 动态阻挡（Static/Kinematic body）

机制：运行时加盒子/柱体；CharacterVirtual / ShapeCast 挡位移。

| 维度 | 评价 |
|------|------|
| AI 绕路 | ❌ **不自动进 Nav**；只放 Jolt 则 AI 仍当平地 |
| 玩家硬碰撞/滑墙 | ✅ 适合 |
| 与异步晚 1 帧管线 | ⚠️ 碰撞校正可延迟；**禁行语义不宜延迟** |
| 精度 | ✅ 几何贴外形 |
| 工程成熟度 | ❌ 当前仅地图 Static Mesh + 实体胶囊，无阻挡增删 API |
| 单独当权威可走性 | ❌ 生产级不够（寻路、校验、表现三者易不一致） |

### 2.3 为什么“只放一边”到不了生产级

```text
只 Nav、永不碰 Jolt
  → AI/校验正确，贴墙可能“直接停住”或位置裁短，手感偏硬（多数动作 MMO 可接受）

只 Jolt、不改 Nav
  → 玩家可能被弹出，怪仍寻路穿进阻挡 → 必炸玩法
  → 移动包/Tick 校验若只信 Nav，还会出现“逻辑可走、物理又挡”的双真相
```

**生产级必须：Nav 为可走性权威；Jolt 最多做几何辅层，且与 Nav 同源注册。**

---

## 3. 推荐总架构（双层同源）

```text
                    ┌─────────────────────────────┐
                    │   DynamicBlockService       │  业务唯一入口
                    │   Add/Remove/TickExpire     │
                    └─────────────┬───────────────┘
                                  │ 一次注册，双写（主+可选辅）
              ┌───────────────────┴───────────────────┐
              ▼                                       ▼
     【主·权威】NavMesh                         【辅·可选】Jolt
     add_obstacle / remove_obstacle            CreateStaticBlocker(box/cyl)
     area=63 禁行                              CharacterVirtual / Cast 滑墙
              │                                       │
              ▼                                       ▼
     AI 寻路 / validate / Tick 积分裁剪        位移接触响应（可晚 1 帧 Apply）
              │
              ▼
     广播 cli_3d_addblock / delblock（表现，客户端不必改本地 Nav）
```

### 设计原则

1. **单一写入点**：玩法只调 `DynamicBlockService`，禁止技能直接改 Nav 或直接加 Jolt body。  
2. **权威在 Nav**：任何“能不能站/能不能穿”的判定以 Nav 为准。  
3. **Jolt 可选开关**：Phase 1 可只开 Nav；Phase 2 再开物理辅层。  
4. **客户端不同步改服务器 Nav**：S→C 只发阻挡描述做表现（与 `obstacle_sync_design.md` 一致）。  
5. **生命周期绑定实体/技能实例**：超时、死亡、技能结束必须 `Remove`，防泄漏。

---

## 4. 与移动 / Tick / 物理管线如何咬合

### 4.1 目标移动模型（生产向）

```text
收包：只更新意图（dir / speed / status）
Tick：
  1. ApplyPhysicsResults（若启用晚 1 帧 Jolt）  // 几何校正，不改“禁行表”
  2. 意图积分 → candidate_pos
  3. Nav 校验（含动态阻挡 poly）→ 合法则采用，否则滑墙裁短/拒绝
  4. （可选）Jolt CharacterVirtual 再细修
  5. MoveEntity + AOI Flush
```

### 4.2 动态阻挡加删时机

| 操作 | 线程 | 说明 |
|------|------|------|
| Add/Remove Nav 阻挡 | **主线程，立即** | 下一拍 AI/校验立刻看见 |
| Add/Remove Jolt body | 主线程（同步）或投物理线程但 **就绪前 Nav 已生效** | 允许物理晚半拍，不允许 Nav 晚半拍 |
| 广播 addblock/delblock | 主线程 | 仅表现 |

**禁止：** 把 `setPolyArea` 丢进异步 Jolt 完成回调才执行。

### 4.3 技能墙 vs 环境墙

| 类型 | Nav | Jolt | 备注 |
|------|-----|------|------|
| 技能召唤墙 / 禁行圈 | ✅ 必加 | 可选 | 短生命周期，要到期删除 |
| 副本门 / 阶段锁 | ✅ 必加 | 可选 | 可与机关实体绑定 |
| 纯表现栏杆（可穿） | ❌ | ❌ | 不要进阻挡服务 |
| 布景物理碎石（可被推） | ❌/弱 | ✅ 动态刚体 | 不进“逻辑阻挡点”表 |

---

## 5. 分阶段落地（到达生产级）

### Phase 0 — 现状对齐（已有）

- [x] `NavMeshHandle` setPolyArea 障碍（上限 256）
- [x] `NavSystem::add_obstacle` / `add_box_obstacle` / `remove_obstacle`
- [x] raycast AABB 补盲
- [ ] 业务零调用 → **必须接线**

### Phase 1 — 生产最小闭环（只做 Nav，必须先上）

目标：怪会绕、玩家穿不过、客户端看得到。

1. 新增 `DynamicBlockService`（挂 World 或 Map 实例）  
   - `AddCylinder` / `AddBox` / `Remove` / `RemoveByOwner`  
   - 内部只调 `NavSystem`  
2. 技能/机关调用 Service，不写 Nav API  
3. 玩家位移路径（现 `MoveHandler` 或未来 Tick 积分）统一走 `validate_*`（已含障碍）  
4. NPC `MoveSystem` / AI 寻路使用同一 Nav（自动绕开）  
5. S→C：`cli_3d_addblock` / `cli_3d_delblock`（按 `obstacle_sync_design.md`）  
6. 单测：加阻挡后 findPath 绕开；validate 拒绝穿入；删阻挡恢复  
7. 指标：障碍数量、Add 耗时、泄漏（owner 销毁未 Remove）

**Phase 1 即可称“逻辑动态阻挡生产可用”。**

### Phase 2 — Tick 权威移动（与阻挡强相关）

1. 移动包改为意图；Tick 积分  
2. 积分后强制 Nav（含动态阻挡）校正  
3. 客户端预测 + 权威和解  

### Phase 3 — Jolt 几何辅层（可选增强）

1. `JoltServer::CreateStaticBlocker` / `RemoveBlocker`  
2. Service 在 Add Nav 成功后双写 Jolt（失败打点，不回滚 Nav 或策略性回滚）  
3. 玩家控制器升级 `CharacterVirtual`（或 ShapeCast）  
4. 若用异步物理：仅几何 Apply 晚 1 帧；Nav 仍同步  

### Phase 4 — 规模与表现

- 障碍预算：每图上限、同技能合并、AOI 内才广播 addblock  
- 精度不够时局部加密烘焙或少数关键墙用 box 精确 poly 查询  
- 仍不建议默认上 dtTileCache（除非有明确精度 SLA）

---

## 6. DynamicBlockService 接口草案

```text
struct BlockDesc {
  enum Type { Cylinder, Box };
  Type type;
  Vector3 center_or_min;
  float radius, height;      // cylinder
  Vector3 max;               // box
  uint64_t owner_entity_id;  // 技能/机关实体
  uint32_t expire_ms;        // 0 = 手动删
};

class DynamicBlockService {
  // 返回 block_id（对外广播用）；内部映射 nav_ref [+ jolt_body]
  uint64_t Add(map_id, map_instance, BlockDesc);
  void Remove(uint64_t block_id);
  void RemoveByOwner(uint64_t entity_id);
  void Tick(float dt);       // 到期清理
};
```

规则：

- `Add` 失败（超上限、无 mesh）→ 技能应失败或降级，不能“假装有墙”。  
- `Remove` 幂等。  
- 换图/卸载 Nav 前 `clear` 全部记录（现有 `clearObstacles` 语义）。

---

## 7. 决策表（开会可直接用）

| 问题 | 答案 |
|------|------|
| 动态阻挡主放哪？ | **NavMesh（setPolyArea）** |
| Jolt 要不要？ | **可选辅层**，Phase 3；不能单飞 |
| 异步 Jolt 晚 1 帧？ | 只影响几何校正；**禁行标记必须同步 Nav** |
| 客户端改本地 Nav 吗？ | **不改**；只收 addblock 表现 |
| 现有代码够不够 Phase 1？ | **够**（差业务 Service + 协议广播 + 测试） |
| 和龙之谷生产级关系？ | Phase 1 是门槛；Phase 2 移动权威；Phase 3 手感 |

---

## 8. 一句话

**生产级动态阻挡 = NavMesh 权威禁行（必做） + 协议表现同步 +（可选）Jolt 同源几何辅层；**  
AI、Tick 位移、反穿模都认 Nav；Jolt 只提升贴墙手感，不独占“挡点”语义。
