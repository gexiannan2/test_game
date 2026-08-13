# CODEBUDDY.md This file provides guidance to CodeBuddy when working with code in this repository.

> **Workspace root**: `C:\e996_proj-nav\` (WSL: `/mnt/c/e996_proj-nav/`).  
> When opening this project in VS Code, use **File → Open Folder** and select `C:\e996_proj-nav\` as the workspace root to ensure the full `src/` C++ source tree is included.

## Build System

本项目使用 **CMake + Ninja** 构建。根 `CMakeLists.txt` 位于项目根目录。

- **完整构建（WSL）**: `./wsl_build.sh` — 等价于 `cmake -S . -B build/wsl -G Ninja && cmake --build build/wsl`
- **构建指定模块**: `./wsl_build.sh <target> [debug|release]`，target 可选值取决于 CMakeLists.txt 定义（如 `3dgame`、`all`）
- **单独构建 3dgame 动态库**: `./build_3dgame.sh [debug|release]` — 产出 `release/bin/lib3dgame.so`
- **单模块 CMake 构建**: 各模块 (`src/3dgame/`, `src/svc_cache/`, `src/svc_gate/`, `src/svc_log/`, `src/svc_login/`) 有独立 `CMakeLists.txt`，可单独 `cmake -S src/<module> -B build/<module>` 构建
- **构建目录**: 所有构建输出在 `build/wsl/` 下
- **`package/` 目录**: 包含 xmake 二进制文件，但项目实际不使用 xmake（无 `xmake.lua`），仅为遗留遗留

注意：项目没有 `src/xmake.lua`，也没有顶层 `src/CMakeLists.txt`，所有 xmake 相关命令不可用。

---

# 项目说明

这是一个 C++ 服务端项目，主要关注服务器稳定性、内存安全、线程安全和性能问题。

# 技术栈

- C++20
- CMake + Ninja
- Linux 服务端
- 可能包含 Lua C API、dlopen/dlsym 插件加载、MySQL、Redis、网络通信模块

# 工作规则

- 不要直接大面积修改代码。
- 修改代码前，先输出问题分析和修改方案。
- 不要自动执行 git commit。
- 不要修改 third_party、external、vendor 目录。
- 不要格式化整个仓库，只能格式化被修改的文件。
- 修复内存问题时，优先给出最小修改方案。

## 复杂工程任务处理流程

当用户要求分析复杂 C++ 工程、疑难 bug、core dump、编译错误、仓库级修改、工具调用或长链路任务时，CodeBuddy 必须按以下流程执行：

1. 先不要直接修改代码。
2. 先理解用户指定文件、相关文件和模块的职责。
3. 梳理关键调用链，包括入口函数、核心分支、数据流、对象生命周期和错误传播路径。
4. 根据日志、core dump、gdb 堆栈、编译错误或运行现象，提出 3 个最可能原因。
5. 将 3 个原因按概率从高到低排序，并说明判断依据。
6. 优先选择最小改动方案，避免大范围重构。
7. 修改代码前，必须先告诉用户：
   - 准备修改哪些文件；
   - 每个文件为什么需要修改；
   - 修改会影响哪些调用链；
   - 是否涉及线程安全、内存生命周期、ABI、性能或坐标系风险。
8. 用户确认后再修改代码。
9. 修改后必须给出：
   - 修改点说明；
   - 编译命令；
   - 测试点；
   - 回滚风险；
   - 是否建议使用 ASan、UBSan、TSan、Valgrind、gdb 或日志验证。

对于疑难 bug，不允许只给结论，必须先给出排查路径和证据链。对于工程代码修改，不允许一次性大面积改动，必须采用小步修改、小步验证的方式。

# 内存问题检查重点

请重点检查：

- use-after-free
- double free
- 野指针
- 悬垂引用
- lambda 捕获 this 后对象提前释放
- new/delete、new[]/delete[] 不匹配
- malloc/free 不匹配
- std::vector 扩容后旧指针/引用/迭代器失效
- memcpy、strcpy、sprintf 越界
- buffer offset 解析越界
- FILE*、MYSQL_RES、redisReply、luaL_ref 等资源是否成对释放
- 多线程回调、异步任务、网络连接对象生命周期问题
- dlopen/dlsym 插件边界的 ABI 和跨模块释放问题

# 输出要求

扫描代码时，请按以下格式输出：

- 文件路径
- 函数名
- 大概行号
- 风险级别：高 / 中 / 低
- 问题原因
- 触发条件
- 修复建议
- 是否建议用 ASan / Valgrind 验证




# CodeBuddy 项目代码规范

> 本文件用于约束 CodeBuddy 在本项目中生成、修改、重构 C++ 代码时的代码风格。
> 修改任何代码前，必须先读取并遵守本文件。
> 如果生成结果违反本文件，必须重新生成。

---

## 1. 总体原则

本项目代码风格以 **可读性、稳定性、低风险修改** 为第一优先级。

CodeBuddy 在本项目中工作时，必须遵守以下原则：

1. 不要为了减少行数压缩代码。
2. 不要把多行代码改成单行代码。
3. 不要随意重命名已有变量、函数、类、文件。
4. 不要随意调整已有接口、ABI 边界、导出宏、回调签名。
5. 不要做与用户要求无关的大范围重构。
6. 修改代码时优先保持现有逻辑不变。
7. 如果只是修 bug，只修相关代码，不要顺手改无关逻辑。
8. 新增代码必须和本文件的代码风格一致。
9. 修改已有代码时，如果附近存在明显违反本规范的控制语句，应顺手修正。
10. 输出代码前必须自检格式，尤其是 `if` / `for` / `while` / `switch`。

---

## 2. C++ 花括号风格

本项目统一使用 **Allman 风格**。

左花括号 `{` 必须单独占一行。

### 正确示例

```cpp
if (ret)
{
    return false;
}

for (int i = 0; i < count; ++i)
{
    DoSomething(i);
}

while (running)
{
    Update();
}
```

### 错误示例

```cpp
if (ret) {
    return false;
}

for (int i = 0; i < count; ++i) {
    DoSomething(i);
}

while (running) {
    Update();
}
```

---

## 3. if / else 代码风格

### 3.1 if 后必须换行

`if` 后面的执行体必须换行，禁止写在同一行。

### 正确示例

```cpp
if (obj == nullptr)
{
    return false;
}
```

### 错误示例

```cpp
if (obj == nullptr) return false;
```

---

### 3.2 if 哪怕只有一行，也必须使用花括号

禁止省略花括号。

### 正确示例

```cpp
if (ret)
{
    return false;
}

if (obj == nullptr)
{
    continue;
}

if (value <= 0)
{
    break;
}
```

### 错误示例

```cpp
if (ret)
    return false;

if (obj == nullptr)
    continue;

if (value <= 0)
    break;
```

---

### 3.3 禁止单行 if

禁止任何形式的单行 `if`。

### 错误示例

```cpp
if (1) continue;

if (ret) return false;

if (obj == nullptr) break;

if (error != 0) goto failed;
```

### 正确示例

```cpp
if (1)
{
    continue;
}

if (ret)
{
    return false;
}

if (obj == nullptr)
{
    break;
}

if (error != 0)
{
    goto failed;
}
```

---

### 3.4 else / else if 风格

`else` 和 `else if` 必须单独换行。

### 正确示例

```cpp
if (ret)
{
    return false;
}
else
{
    return true;
}
```

```cpp
if (type == 1)
{
    HandleType1();
}
else if (type == 2)
{
    HandleType2();
}
else
{
    HandleDefault();
}
```

### 错误示例

```cpp
if (ret)
{
    return false;
} else {
    return true;
}
```

```cpp
if (type == 1) HandleType1();
else if (type == 2) HandleType2();
else HandleDefault();
```

---

## 4. for 循环代码风格

### 4.1 for 后必须换行

`for` 后面的执行体必须换行，禁止写在同一行。

### 正确示例

```cpp
for (int i = 0; i < count; ++i)
{
    DoSomething(i);
}
```

### 错误示例

```cpp
for (int i = 0; i < count; ++i) DoSomething(i);
```

---

### 4.2 for 哪怕只有一行，也必须使用花括号

### 正确示例

```cpp
for (auto& player : players)
{
    player.Update();
}
```

### 错误示例

```cpp
for (auto& player : players)
    player.Update();
```

---

### 4.3 range-for 也必须遵守相同规则

### 正确示例

```cpp
for (const auto& item : items)
{
    ProcessItem(item);
}
```

### 错误示例

```cpp
for (const auto& item : items) ProcessItem(item);
```

---

## 5. while / do while 代码风格

### 正确示例

```cpp
while (running)
{
    Update();
}
```

```cpp
do
{
    ReadNext();
}
while (HasMore());
```

### 错误示例

```cpp
while (running) Update();

while (running)
    Update();

do {
    ReadNext();
} while (HasMore());
```

---

## 6. switch 代码风格

`switch` 必须使用 Allman 风格花括号。

### 正确示例

```cpp
switch (state)
{
case State::Init:
    Init();
    break;

case State::Running:
    Update();
    break;

case State::Stop:
    Stop();
    break;

default:
    break;
}
```

### 错误示例

```cpp
switch (state) {
case State::Init: Init(); break;
case State::Running: Update(); break;
default: break;
}
```

---

## 7. 函数代码风格

函数左花括号必须单独一行。

### 正确示例

```cpp
bool CheckPosition(const Position& pos)
{
    if (!pos.IsValid())
    {
        return false;
    }

    return true;
}
```

### 错误示例

```cpp
bool CheckPosition(const Position& pos) {
    if (!pos.IsValid()) return false;
    return true;
}
```

---

## 8. 类、结构体和 namespace 代码风格

类、结构体、命名空间、函数、控制语句的左花括号必须单独一行。

**强制规则：所有块的左花括号 `{` 后第一行（紧接花括号的成员声明、访问说明符、typedef、using、enum 体、函数体第一条语句、if / for / while 体第一条语句等）必须有正确的缩进，禁止顶格或与 `{` 同列。**

### 8.0 花括号后的第一行缩进 — 总则

下面规则对 namespace / class / struct / enum / 函数体 / 控制语句体 一律适用：

1. `{` 单独一行，列对齐于所属块（namespace / class / function 等）的 `{` 所在列。
2. `{` 后**第一行**的可写代码必须有 1 级缩进（4 空格）。
3. 后续行按所在嵌套层级继续缩进；每深 1 层多缩 4 空格。
4. 退出块时（`}` 单独一行），缩进回到块外层级。
5. `}` 与对应 `{` 列对齐；`}` 后可加 `// namespace <name>` / `// class <Name>` 等注释标识。

#### 8.0.1 namespace 的 `{` 后第一行示例

```cpp
namespace e996
{
    // ✓ namespace 的 `{` 后第一行：缩进一级
    class cache_handler : public e996::handler
    {
        // ✓ class 的 `{` 后第一行（access label）：缩进一级
        private:
            // ✓ access label 下的成员：再缩进一级
            std::unordered_map<std::uint64_t, on_player_data_loaded_t> m_callbacks;

        public:
            // ✓ access label 与 class 体内部一级缩进对齐
            cache_handler(e996::thread* owner);
            ~cache_handler();
    };
} // namespace e996
```

#### 8.0.2 错误示例（`{` 后第一行顶格）

```cpp
namespace e996
{
class cache_handler : public e996::handler   // 缺少一级缩进 — namespace 的 `{` 后第一行
{
private:
    std::unordered_map<...> m_callbacks;
public:
    cache_handler(...);
};
}
```

### 8.1 class / struct 代码风格

class、struct 内部必须使用 4 个空格缩进。访问说明符（`public:` / `private:` / `protected:`）在类体内部缩进**一级**（比 class 体多缩进 4 空格），其下的成员函数、成员变量再缩进**一级**（比访问说明符多缩进 4 空格）。

#### 正确示例

```cpp
class Player
{
    public:
        void Update();

    private:
        int m_id = 0;
};
```

```cpp
struct Position
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};
```

#### 错误示例（缺缩进）

```cpp
class Player
{
public:
void Update();        // 缺少一级缩进
int m_id = 0;         // 缺少一级缩进
};
```

#### 错误示例（花括号不独占一行）

```cpp
class Player {
    void Update();
    int m_id = 0;
};
```

### 8.2 namespace 代码风格

namespace 必须使用 **Allman 风格** 花括号（左花括号独占一行）。

- namespace 内的所有成员（变量、函数、类、枚举、类型别名等）必须缩进**一级**。
- namespace 的 `{` 后**第一行**（紧接花括号的成员声明、typedef、using 等）必须缩进一级（4 空格），禁止顶格或与 `{` 同列。
- namespace 嵌套 class / struct / enum 等时，内部成员必须再缩进一级。
- namespace 多层嵌套时，每进入一层，缩进增加一级。
- 关闭 namespace 的右花括号独占一行，且与 `namespace` 关键字列对齐（不再继续缩进）。

#### 正确示例（单层 namespace）

```cpp
namespace e996
{
    int g_value = 0;

    class Player
    {
        public:
            void Update();

        private:
            int m_id = 0;
    };

    void DoSomething();
} // namespace e996
```

#### 正确示例（多层 namespace 嵌套）

```cpp
namespace game
{
    namespace player
    {
        class Player
        {
            public:
                void Update();
        };
    }
}
```

#### 错误示例（namespace 内部缺少缩进）

```cpp
namespace e996
{
int g_value = 0;            // 缺少一级缩进

class Player
{
public:                     // 缺少一级缩进
void Update();              // 缺少二级缩进
int m_id = 0;               // 缺少二级缩进
};

void DoSomething();
}
```

#### 错误示例（namespace 嵌套时未逐级缩进）

```cpp
namespace game
{
namespace player
{
class Player
{
public:
    void Update();          // 只缩进一级，缺少 namespace 与 class 的两级缩进
};
}
}
```

注意：左花括号 `{` 必须独占一行；右花括号 `}` 也必须独占一行，且与所在块对齐。

---

## 9. 空行规则

### 9.1 控制语句块之间保留空行

### 推荐写法

```cpp
if (player == nullptr)
{
    return false;
}

if (!player->IsAlive())
{
    return false;
}

player->Update();
```

### 不推荐写法

```cpp
if (player == nullptr)
{
    return false;
}
if (!player->IsAlive())
{
    return false;
}
player->Update();
```

---

### 9.2 函数内部逻辑分段使用空行

推荐把不同逻辑段落用空行分开。

### 正确示例

```cpp
bool PlayerManager::AddPlayer(Player* player)
{
    if (player == nullptr)
    {
        return false;
    }

    const uint64_t playerId = player->GetId();
    if (playerId == 0)
    {
        return false;
    }

    m_players[playerId] = player;
    return true;
}
```

---

## 10. 缩进规则

本项目统一使用 **4 个空格** 缩进。

**禁止使用 Tab 作为缩进**（包括源码、注释、宏定义内文本）。

### 10.0 花括号后的第一行缩进

任何块的左花括号 `{` 之后**第一行**的可写代码必须有正确的缩进，禁止顶格或与 `{` 同列。这一条适用于 namespace / class / struct / enum / function / if / for / while / switch / try 等所有块。

错误示例：

```cpp
namespace e996
{
class cache_handler : public handler   // 错误：花括号后第一行未缩进
{
public:
    void on_init();                   // 错误：花括号后第一行未缩进
};
}
```

正确示例：

```cpp
namespace e996
{
    class cache_handler : public handler
    {
        public:
            void on_init();
    };
} // namespace e996
```

```h
namespace e996
{
    class cache_handler : public handler
    {
        public:
            void on_init();
    };
} // namespace e996

### 10.1 缩进级别

每进入一个新的代码块，缩进增加一级（4 个空格）：

- 函数体
- `class` / `struct` / `union` 体
- `namespace` 体
- `if` / `else` / `for` / `while` / `do while` / `switch` 体
- `try` / `catch` 体
- 访问说明符（`public:` / `private:` / `protected:`）下的成员
- enum / enum class 体
- namespace 内部再次嵌套的 namespace / class / struct

退出代码块时，缩进相应减少一级，且必须严格对齐。

### 10.2 正确示例

```cpp
if (ret)
{
    DoSomething();

    if (needUpdate)
    {
        Update();
    }
}
```

```cpp
namespace e996
{
    int g_value = 0;

    class Player
    {
        public:
            void Update();
            int GetId() const;

        private:
            int m_id = 0;
    };
}
```

```cpp
namespace game
{
    namespace player
    {
        class Player
        {
            public:
                void Update();
        };
    }
}
```

### 10.3 错误示例

```cpp
namespace e996
{
int g_value = 0;        // 缺少一级缩进

class Player
{
public:                 // 缺少一级缩进
void Update();          // 缺少二级缩进
int m_id = 0;           // 缺少二级缩进
};
}
```

```cpp
if (ret)
{
DoSomething();         // 缺少一级缩进
}
```

---

## 11. 命名和接口修改约束

CodeBuddy 修改代码时必须注意：

1. 不要随意修改已有函数名。
2. 不要随意修改已有类名。
3. 不要随意修改已有导出接口。
4. 不要随意修改已有回调函数签名。
5. 不要随意修改已有 C API。
6. 不要随意修改已有 `extern "C"` 接口。
7. 不要随意删除 `E996_API`、`__declspec(dllexport)`、`__declspec(dllimport)`、`visibility` 等导出相关宏。
8. 如果确实需要修改接口，必须明确说明原因和影响范围。

---

## 12. 指针和空指针规则

C++ 代码中优先使用 `nullptr`，不要使用 `NULL` 或 `0` 表示空指针。

### 正确示例

```cpp
if (player == nullptr)
{
    return false;
}
```

### 错误示例

```cpp
if (player == NULL)
{
    return false;
}

if (player == 0)
{
    return false;
}
```

---

## 13. const 使用规则

不会被修改的变量、参数、成员函数应尽量使用 `const`。

### 推荐写法

```cpp
bool IsValidPosition(const Position& pos)
{
    return pos.x >= 0.0f && pos.z >= 0.0f;
}
```

```cpp
uint64_t Player::GetId() const
{
    return m_id;
}
```

---

## 14. 早返回风格

允许使用早返回减少嵌套，但早返回也必须遵守花括号规则。

### 正确示例

```cpp
bool CheckPlayer(Player* player)
{
    if (player == nullptr)
    {
        return false;
    }

    if (!player->IsOnline())
    {
        return false;
    }

    return true;
}
```

### 错误示例

```cpp
bool CheckPlayer(Player* player)
{
    if (player == nullptr) return false;
    if (!player->IsOnline()) return false;
    return true;
}
```

---

## 15. 禁止的代码格式

CodeBuddy 绝对不能生成以下格式：

```cpp
if (x) return;

if (x) continue;

if (x) break;

if (x) goto failed;

for (...) DoSomething();

while (...) DoSomething();

if (x)
    return;

for (...)
    DoSomething();

while (...)
    DoSomething();
```

必须改成：

```cpp
if (x)
{
    return;
}

if (x)
{
    continue;
}

if (x)
{
    break;
}

if (x)
{
    goto failed;
}

for (...)
{
    DoSomething();
}

while (...)
{
    DoSomething();
}
```

---

## 16. 修改已有代码时的要求

当用户要求修改某个函数或文件时，CodeBuddy 必须：

1. 只修改与需求相关的代码。
2. 保持原有业务逻辑不变，除非用户明确要求改变。
3. 不要引入无关重构。
4. 不要为了“优化”而改变接口。
5. 不要压缩代码格式。
6. 不要生成单行控制语句。
7. 修改后的代码必须能和原项目风格兼容。
8. 如果改动涉及线程安全、内存释放、生命周期、ABI，必须重点说明风险。
9. 如果改动涉及寻路、NavMesh、高度图、AOI、碰撞检测，必须避免破坏原有坐标系和高度来源逻辑。
10. 如果改动涉及 Lua / C API / 导出库，必须注意 ABI 稳定性。

---

## 17. C++ 服务器项目注意事项

本项目偏 C++ 游戏服务器开发，CodeBuddy 生成代码时应额外注意：

1. 注意内存生命周期，避免野指针、悬空引用、重复释放。
2. 注意容器迭代器失效。
3. 注意多线程访问共享数据时的锁保护。
4. 注意回调函数中对象是否仍然存活。
5. 注意跨 DLL / SO 边界的 ABI 稳定性。
6. 注意 C API 暴露接口不要使用不稳定的 C++ 类型。
7. 注意 Lua 回调、注册表引用、`lua_State*` 的生命周期。
8. 注意 NavMesh、高度图、AOI 等系统中的坐标轴含义。
9. 注意 `float` 精度误差，不要直接用 `==` 比较浮点数。
10. 注意日志不要过多影响服务器性能。

---

## 18. 生成代码前的自检清单

CodeBuddy 每次输出 C++ 代码前，必须检查是否存在以下问题：

- 是否存在 `if (...) return ...;`
- 是否存在 `if (...) continue;`
- 是否存在 `if (...) break;`
- 是否存在 `if (...) goto ...;`
- 是否存在 `for (...) xxx;`
- 是否存在 `while (...) xxx;`
- 是否存在没有 `{}` 的 `if`
- 是否存在没有 `{}` 的 `for`
- 是否存在没有 `{}` 的 `while`
- 是否存在 `{` 和 `if / for / while / else / function` 在同一行
- 是否把已有多行代码压缩成单行
- 是否修改了无关代码
- 是否改变了已有接口
- 是否引入了 ABI 风险
- **namespace 内的成员（变量、函数、类、枚举、类型别名）是否缩进一级**
- **namespace 嵌套 class / struct 时，内部成员是否逐级缩进**
- **class / struct 内部的访问说明符下的成员是否缩进一级**
- **`namespace` / `class` / `struct` / `function` 等的左花括号是否独占一行**
- **`namespace` / `class` / `struct` / `function` 等块的 `{` 后第一行是否有正确的缩进（禁止顶格或与 `{` 同列）**

只要发现上述问题，必须先修正，再输出最终代码。

---

## 19. 推荐配套 .clang-format

如果项目根目录存在 `.clang-format`，CodeBuddy 生成代码时也必须遵守它。

推荐配置如下：

```yaml
BasedOnStyle: Microsoft
IndentWidth: 4
TabWidth: 4
UseTab: Never

BreakBeforeBraces: Allman

AllowShortIfStatementsOnASingleLine: Never
AllowShortLoopsOnASingleLine: false
AllowShortBlocksOnASingleLine: Never
AllowShortFunctionsOnASingleLine: None
AllowShortCaseLabelsOnASingleLine: false

ColumnLimit: 120
```

---

## 20. 给 CodeBuddy 的强制执行指令

当用户要求生成或修改代码时，必须按照以下规则执行：

```text
修改代码前，先读取项目根目录下的 codebuddy.md 和 .clang-format。
所有生成和修改的 C++ 代码必须符合 codebuddy.md。
禁止生成单行 if / for / while。
所有控制语句即使只有一行，也必须换行并使用花括号。
左花括号必须独占一行。
如果输出代码违反规则，必须重新生成。
```

---

## 21. 最核心规则

如果只记住几条，必须记住下面这些：

```text
if 后必须换行。
for 后必须换行。
while 后必须换行。
所有控制语句必须使用花括号。
禁止单行 if / for / while。
左花括号必须独占一行。
不要把已有多行代码压缩成单行。
不要随意修改接口和 ABI 边界。
namespace / class / struct 内的成员必须逐级缩进。
namespace 内的所有代码（变量、函数、类）都必须缩进一级。
所有块的左花括号 `{` 后第一行必须有正确的缩进，禁止顶格或与 `{` 同列。
```
