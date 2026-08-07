# 每日记忆文档更新提示词

> **用法**：每天下班复制下面「执行块」到 Cursor Agent（或 `/loop` 定时），自动扫描 `src/` 并刷新 `doc/memory/` 全部记忆文件。  
> **前提**：WSL 或 Linux 环境可编译运行 `./bin/svc_game_3d_test`。

---

## 执行块（复制从这里开始）

```
你是 svc_game_3d 项目的文档维护 Agent。请执行「记忆文档日更」：

## 目标
递归扫描 `src/`（client / common / protocol / server），对照源码更新 `doc/memory/` 下全部 `.md`，确保 AI 下次提问能快速定位问题。

## 步骤

### 1. 扫描与验证
- 列出 `src/` 核心模块变更（git diff 最近 7 天或全量结构扫描）
- 在 WSL 运行：
  cd /mnt/d/Dev/tmp1/game && make -j$(nproc) svc_game_3d_test && ./bin/svc_game_3d_test
- 记录：通过数 / 失败数 / 失败用例名

### 2. 更新记忆文件（必须全部 touch）
更新以下文件，每个文件顶部 `最后扫描：YYYY-MM-DD` 改为今天：

- doc/memory/INDEX.md
- doc/memory/flow-connection.md
- doc/memory/flow-login.md
- doc/memory/flow-role.md
- doc/memory/flow-enter-game.md
- doc/memory/flow-move-aoi.md
- doc/memory/flow-disconnect-reconnect.md
- doc/memory/flow-world-tick.md
- doc/memory/flow-persist.md
- doc/memory/bugs-and-constraints.md
- doc/memory/file-locator.md

每个 flow 文件须包含：
1. **关键词**（便于 @ 搜索）
2. **调用链**（箭头格式，文件::函数）
3. **关键文件表**
4. **硬约束**（与 doc/server/constraints.md 一致）
5. **相关测试** + 过滤命令
6. 如有新 bug 修复 → 写入 bugs-and-constraints.md

### 3. 同步权威文档
若发现 flow 与 `doc/server/*.md` 不一致：
- 以 **源码为准**
- 同时更新 `doc/server/changelog-bugs.md` 和 `doc/server/记忆.md` §3

### 4. 新 bug 处理
- 发现 bug → 修代码 + 补测试（若合适）→ 写入 changelog
- 仅文档过时 → 只更新 md

### 5. 输出摘要
完成后回复：
- 扫描日期
- 测试结果 (N/N)
- 变更的记忆文件列表
- 新发现/新修复 bug 条目
- 已知未接线项是否有变化

## 约束
- 不要改无关代码
- 记忆文件保持精简（每 flow < 120 行）
- 中文撰写，代码路径用反引号
- 不要 commit，除非用户明确要求
```

---

## 可选：Cursor Automations 定时

若已安装 Automations 技能，可创建规则：

- **触发**：工作日 18:00  
- **动作**：运行上述执行块  
- **分支**：当前 main/develop

---

## 快速手动触发（仅跑测试 + 改日期）

```bash
cd /mnt/d/Dev/tmp1/game
make -j$(nproc) svc_game_3d_test && ./bin/svc_game_3d_test
# 然后让 AI：「按 UPDATE_PROMPT.md 刷新 doc/memory/ 日期与测试结果」
```

---

## 记忆目录结构

```
doc/memory/
├── INDEX.md              ← AI 入口，先读这个
├── flow-*.md             ← 8 个核心流程
├── bugs-and-constraints.md
├── file-locator.md
└── UPDATE_PROMPT.md      ← 本文件
```
