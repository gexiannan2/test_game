# svc_game_3d 构建与运行文档

> 适用环境: WSL2 (Ubuntu 24.04, GCC 13.3, CMake 3.28, GNU Make 4.3)
> 构建方式: in-source build, 默认 Unix Makefiles 生成器

---

## 一、CMake 构建 (Release / Debug)

### 1.1 基本工作流

工程采用 **in-source build**（构建产物落在工程根目录），不需要单独的 `build/` 子目录。

```bash
cd <project_root>
cmake .                 # 生成 Makefile (首次 / 改 CMakeLists.txt 后)
make -j$(nproc)         # 全量编译
make svc_game_3d_server # 只编译 server 目标
make clean              # 清 .o, 保留 Makefile
```

### 1.2 Release 构建 (默认)

`CMakeLists.txt` 已设默认 `CMAKE_BUILD_TYPE=Release`，直接：

```bash
cd <project_root>
cmake . -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

Release 模式编译器开关 (见 `CMakeLists.txt` 第 28 行 `set(CMAKE_BUILD_TYPE Release ...)`):
- `-O3 -DNDEBUG` (CMake 自动加)
- 不含 `-g`, 体积小、运行快

### 1.3 Debug 构建

```bash
cd <project_root>
cmake . -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
```

Debug 模式编译器开关:
- `-g -O0` (CMake 自动加, 含调试信息, 不优化)
- 可用 `gdb ./bin/svc_game_3d_server` 调试
- 可用 `valgrind --leak-check=full ./bin/svc_game_3d_server` 查内存问题

> **注意**: `CMAKE_BUILD_TYPE` 是 CMake cache 变量, 改值后必须重跑 `cmake .` (或 `rm CMakeCache.txt && cmake .`)。`make` 单独不会切换 build type。

### 1.4 常用 cmake 参数

| 参数 | 说明 | 示例 |
|------|------|------|
| `-DCMAKE_BUILD_TYPE=Release` | Release (-O3 -DNDEBUG) | 默认 |
| `-DCMAKE_BUILD_TYPE=Debug` | Debug (-g -O0) | 调试用 |
| `-DCMAKE_BUILD_TYPE=RelWithDebInfo` | 带调试信息的优化 (-O2 -g) | 性能分析 |
| `-DCMAKE_BUILD_TYPE=MinSizeRel` | 最小体积 (-Os) | 嵌入式 |
| `-DCMAKE_C_COMPILER=clang` | 用 clang 替代 gcc | `cmake . -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++` |
| `-DCMAKE_VERBOSE_MAKEFILE=ON` | make 时打印完整编译命令 | 排查编译选项 |

例: 切换 clang + RelWithDebInfo + 打印命令:
```bash
rm -f CMakeCache.txt
cmake . -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_VERBOSE_MAKEFILE=ON
make -j$(nproc)
```

### 1.5 彻底清理 (重新配置)

```bash
# 只清 .o (保留 Makefile, 下次直接 make)
make clean

# 彻底清理 CMake 产物 (下次必须重跑 cmake .)
rm -f Makefile cmake_install.cmake CMakeCache.txt
rm -rf CMakeFiles
rm -f bin/svc_game_3d_*            # 可选: 也清掉产物
```

### 1.6 build.sh 封装脚本

不想敲长命令可用 `build.sh`:

| 命令 | 作用 |
|------|------|
| `./build.sh` | Release 全量编译 |
| `./build.sh debug` | Debug 全量编译 (会重跑 cmake) |
| `./build.sh server` | 只编 `svc_game_3d_server` |
| `./build.sh client` | 只编 `svc_game_3d_client` |
| `./build.sh stress` | 只编 `svc_game_3d_stress_test` |
| `./build.sh pingpong` | pingpong server+client |
| `./build.sh bench` | EventLoop 基准 |
| `./build.sh clean` | make clean |
| `./build.sh distclean` | 彻底清理 (删 Makefile 等) |
| `./build.sh reconfigure` | 删 CMakeCache.txt 后重跑 cmake (改 CMakeLists 后) |
| `./build.sh proto` | 重新生成 proto 代码 (调 Windows `gen.bat`) |

### 1.7 编译产物

| 目标 | 输出 | 依赖 |
|------|------|------|
| `svc_game_3d_server` | `bin/svc_game_3d_server` | zrpc + jolt + protobuf |
| `svc_game_3d_client` | `bin/svc_game_3d_client` | zrpc + protobuf |
| `svc_game_3d_stress_test` | `bin/svc_game_3d_stress_test` | zrpc + protobuf |
| `svc_game_3d_pingpong_server` | `bin/svc_game_3d_pingpong_server` | zrpc (无 protobuf) |
| `svc_game_3d_pingpong_client` | `bin/svc_game_3d_pingpong_client` | zrpc (无 protobuf) |
| `svc_game_3d_bench` | `bin/svc_game_3d_bench` | zrpc (EventLoop pipe) |

所有可执行文件输出到 `bin/` 目录。`libprotobufd.so*` 会被 `CMakeLists.txt` 第 72-74 行 `file(COPY ...)` 自动拷贝到 `bin/`, 加上 `-Wl,-rpath,$ORIGIN`, 运行时直接从同级目录找 `.so`。

### 1.8 重新生成 proto 代码

修改 `.proto` 后:

**Windows cmd**:
```
cd /d <project_root>\protobuf
gen.bat
```

**WSL**:
```bash
cd <project_root>
./build.sh proto
```

产物位置 (`gen.bat` 的输出):
- C++ 代码: `protobuf/protos/c++/*.pb.cc` / `*.pb.h` / `*.auto.cpp`
- Lua 描述文件: `protobuf/protos/lua/*.pb`
- Lua 消息 ID 注册表: `protobuf/protos/Msg_Svr_protobuf.lua` / `Msg_Cli_protobuf.lua`

`CMakeLists.txt` 直接引用 `protobuf/protos/c++/` 目录 (单一数据源, 无需手动同步), `gen.bat` 跑完直接 `make` 即可用上新代码。

---

## 二、Linux TCP 连接数优化

### 2.1 查看当前限制

```bash
# 文件描述符上限 (单进程能打开的 fd 数, 含 socket)
ulimit -n

# 系统全局 fd 上限
cat /proc/sys/fs/file-max

# 当前已用 fd 数
cat /proc/sys/fs/file-nr

# TCP 端口范围 (默认 32768-60999, 约 2.8 万个端口)
cat /proc/sys/net/ipv4/ip_local_port_range

# TCP 全连接队列上限 (LISTEN socket 的 accept 队列)
cat /proc/sys/net/core/somaxconn

# 半连接队列上限 (SYN queue)
cat /proc/sys/net/ipv4/tcp_max_syn_backlog

# TIME_WAIT 状态连接数上限
cat /proc/sys/net/ipv4/tcp_max_tw_buckets

# 各 TCP 状态连接数统计
ss -s
ss -tan state time-wait | wc -l
ss -tan state established | wc -l
```

### 2.2 临时调优 (重启失效)

```bash
# 单进程 fd 数提到 100 万
ulimit -n 1048576

# 端口范围扩到 1024-65535 (约 6.4 万个)
echo "1024 65535" > /proc/sys/net/ipv4/ip_local_port_range

# 全连接队列提到 65535 (默认 4096 或 128)
echo 65535 > /proc/sys/net/core/somaxconn

# 半连接队列提到 65535
echo 65535 > /proc/sys/net/ipv4/tcp_max_syn_backlog

# TIME_WAIT 上限提到 1048576 (默认约 4096, 压测必调)
echo 1048576 > /proc/sys/net/ipv4/tcp_max_tw_buckets

# 开启 TIME_WAIT 端口复用 (压测频繁断连必开, 避免端口耗尽)
echo 1 > /proc/sys/net/ipv4/tcp_tw_reuse
# 注: tcp_tw_recycle 已在 Linux 4.12 后被移除, 不要用

# 关闭慢启动重启 (长连接保活时建议关闭)
echo 0 > /proc/sys/net/ipv4/tcp_slow_start_after_idle

# TCP keepalive 探测间隔 (秒), 默认 7200 (2小时), 压测建议 600 (10分钟)
echo 600 > /proc/sys/net/ipv4/tcp_keepalive_time
```

### 2.3 永久调优 (写 sysctl 配置)

```bash
sudo tee /etc/sysctl.d/99-tcp-tuning.conf <<'EOF'
# 单进程 fd 上限 (用户级, 还需配 limits.conf)
# 见 /etc/security/limits.conf

# 端口范围
net.ipv4.ip_local_port_range = 1024 65535

# 连接队列
net.core.somaxconn = 65535
net.ipv4.tcp_max_syn_backlog = 65535

# TIME_WAIT
net.ipv4.tcp_max_tw_buckets = 1048576
net.ipv4.tcp_tw_reuse = 1

# TCP keepalive (压测可调小, 生产保持默认)
net.ipv4.tcp_keepalive_time = 600
net.ipv4.tcp_keepalive_intvl = 30
net.ipv4.tcp_keepalive_probes = 3

# 关闭慢启动重启
net.ipv4.tcp_slow_start_after_idle = 0

# 文件描述符全局上限
fs.file-max = 2097152

# TCP 缓冲区 (压测大吞吐量建议调大, 单位字节)
# min / default / max
net.ipv4.tcp_rmem = 4096 87380 16777216
net.ipv4.tcp_wmem = 4096 65536 16777216
net.core.rmem_max = 16777216
net.core.wmem_max = 16777216

# backlog (netdev_rx_queue_len, 网卡收包队列)
net.core.netdev_max_backlog = 5000
EOF

sudo sysctl -p /etc/sysctl.d/99-tcp-tuning.conf
```

### 2.4 提升 ulimit -n (单进程 fd 上限)

`sysctl` 的 `fs.file-max` 是全局上限, 单进程还要看 `ulimit -n`:

```bash
# 临时 (当前 shell 生效)
ulimit -n 1048576

# 永久 (写 limits.conf, 需要 root)
sudo tee -a /etc/security/limits.conf <<'EOF'
*               soft    nofile          1048576
*               hard    nofile          1048576
root            soft    nofile          1048576
root            hard    nofile          1048576
EOF
```

WSL2 注意: WSL 的 PAM 配置可能不读 `limits.conf`, 重启 WSL 后 `ulimit -n` 仍为默认值 (1024 或 65536)。解决:

```bash
# 在 ~/.bashrc 末尾加
echo 'ulimit -n 1048576' >> ~/.bashrc
source ~/.bashrc
```

或在 `/etc/profile.d/` 下放脚本:
```bash
sudo tee /etc/profile.d/ulimit.sh <<'EOF'
ulimit -n 1048576
EOF
```

### 2.5 WSL2 网络性能注意

WSL2 用 Hyper-V 虚拟网卡, 网络栈比原生 Linux 多一层转发, 性能受限:
- WSL2 本地回环 (`127.0.0.1`): ~400 MB/s
- Windows IOCP 直连: ~2.4 GB/s (6 倍差距)

压测 5000+ 连接时, WSL2 可能成为瓶颈。生产环境建议用原生 Linux 物理机或 Docker 容器。

### 2.6 压测前自检脚本

```bash
# 在 server 端跑 (WSL 内)
echo "=== 系统限制自检 ==="
echo "ulimit -n: $(ulimit -n)"
echo "somaxconn: $(cat /proc/sys/net/core/somaxconn)"
echo "tcp_max_syn_backlog: $(cat /proc/sys/net/ipv4/tcp_max_syn_backlog)"
echo "tcp_max_tw_buckets: $(cat /proc/sys/net/ipv4/tcp_max_tw_buckets)"
echo "tcp_tw_reuse: $(cat /proc/sys/net/ipv4/tcp_tw_reuse)"
echo "port_range: $(cat /proc/sys/net/ipv4/ip_local_port_range)"
echo "file-max: $(cat /proc/sys/fs/file-max)"
echo ""
echo "=== 当前连接数 ==="
ss -s
```

### 2.7 压测目标 (5000 玩家参考值)

| 指标 | 阈值 | 说明 |
|------|------|------|
| `ulimit -n` | ≥ 65535 | server 进程至少要打开 5000 socket + 文件 + 内部 fd |
| `somaxconn` | ≥ 4096 | 防 SYN flood 或瞬时大量连接丢包 |
| `tcp_max_tw_buckets` | ≥ 100000 | churn 模式频繁断连会产生大量 TIME_WAIT |
| `tcp_tw_reuse` | = 1 | 端口复用, 防端口耗尽 |
| `ip_local_port_range` | 1024-65535 | 客户端压测端需要大量源端口 |

---

## 三、单元测试与功能验证

### 3.1 当前可用的"测试程序"

工程当前**未集成 gtest 等正式单元测试框架**, 但有几个可执行的验证程序 (在 `bin/` 下):

| 程序 | 类型 | 用途 |
|------|------|------|
| `svc_game_3d_stress_test` | 压测 | 模拟大量客户端连接, 验证 server 并发能力 |
| `svc_game_3d_pingpong_server` | echo 服务 | 纯 TCP 回显, 测网络吞吐 |
| `svc_game_3d_pingpong_client` | echo 客户端 | 配合 pingpong_server 测吞吐 |
| `svc_game_3d_bench` | 基准 | EventLoop pipe 性能基准 |
| `svc_game_3d_server` | 业务 server | 配合 `svc_game_3d_client` 做端到端验证 |
| `svc_game_3d_client` | 业务 client | 单实例登录走完整流程 |

### 3.2 端到端验证流程 (业务 server + client)

```bash
# 终端 1: 启动 server
cd <project_root>
./bin/svc_game_3d_server 20002
# 或 Debug: gdb ./bin/svc_game_3d_server
#         run 20002

# 终端 2: 启动单客户端, 走完整登录流程
./bin/svc_game_3d_client 127.0.0.1 20002

# 终端 3: 查看连接
ss -tan 'sport = :20002'
```

### 3.3 压测操作 (stress_test)

#### 3.3.1 normal 模式 (默认, 上线后保持心跳)

```bash
# 启动 server (终端 1)
./bin/svc_game_3d_server 20002

# 1000 客户端同时登录, 上线后保持心跳, 永不退出 (终端 2)
./bin/svc_game_3d_stress_test 1000 127.0.0.1 20002 normal

# 每 2 秒打印统计: online/total/heartbeat/kickoff
# 期望: online 数稳定在 1000, heartbeat 每 10s 增长 1000
```

#### 3.3.2 churn 模式 (频繁上下线)

```bash
# 100 客户端, 随机在线后主动断开→重连→循环
./bin/svc_game_3d_stress_test 1000 127.0.0.1 20002 churn

# 期望: reconnect 数持续增长, online 数在 1000 附近波动
# 用于验证重连逻辑 + TIME_WAIT 处理
```

#### 3.3.3 kickoff 模式 (顶号)

```bash
# 100 客户端共享 20 个 uid, 反复顶号登录
./bin/svc_game_3d_stress_test 1000 127.0.0.1 20002 kickoff stress_ 500

# 期望: kickoff 数持续增长, online 数稳定在 20 (因为只有 20 个 uid)
# 用于验证顶号踢人逻辑 + cli_kickoff_player 消息下发
```

#### 3.3.4 压测期间监控

```bash
# server 端连接数 (终端 3)
watch -n 1 'ss -tan "sport = :20002" | tail -n +2 | wc -l'

# server 端进程 fd 数 (终端 4)
watch -n 1 'ls /proc/$(pgrep svc_game_3d_server)/fd | wc -l'

# server 端 CPU/内存
top -p $(pgrep svc_game_3d_server)

# TIME_WAIT 状态连接数
watch -n 1 'ss -tan state time-wait | wc -l'

# 系统级 TCP 统计
ss -s
```

### 3.4 pingpong 吞吐测试

```bash
# 终端 1: 启动 pingpong echo server
./bin/svc_game_3d_pingpong_server 20003

# 终端 2: 单连接 echo 测吞吐
./bin/svc_game_3d_pingpong_client 127.0.0.1 20003 1 100000

# 终端 3: 多连接 (如 100 连接 × 每连接 10000 包)
./bin/svc_game_3d_pingpong_client 127.0.0.1 20003 100 10000

# 输出: 总收发字节数 + QPS + MB/s
```

### 3.5 EventLoop 基准测试

```bash
# 测 EventLoop pipe 的消息投递吞吐 (不涉及网络)
./bin/svc_game_3d_bench
# 输出: ops/sec (每秒投递次数)
```

### 3.6 内存与崩溃检查

```bash
# Debug 构建后用 valgrind 查内存泄漏
cmake . -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)

# 内存泄漏检测 (server 端跑一段时间后 Ctrl+C, 看报告)
valgrind --leak-check=full --show-leak-kinds=all \
         ./bin/svc_game_3d_server 20002

# 用 client 连一下触发各种逻辑
./bin/svc_game_3d_client 127.0.0.1 20002

# AddressSanitizer (编译时加, 运行时检测内存越界/use-after-free)
cmake . -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
        -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
make -j$(nproc)
./bin/svc_game_3d_server 20002
# ASan 报错会打印详细堆栈, 触发问题时自动 abort
```

### 3.7 引入 gtest 框架 (可选, 写正式单元测试)

若要为 `protocol/` `server/handlers/` `server/systems/` 等模块写正式单元测试, 推荐用 GoogleTest:

#### 3.7.1 安装 gtest

```bash
sudo apt install libgtest-dev libgmock-dev -y
# Ubuntu 24.04 的 libgtest-dev 只提供源码, 需编译:
cd /usr/src/gtest
sudo cmake .
sudo make
sudo mv lib/libgtest.a lib/libgtest_main.a /usr/lib/
sudo mv lib/libgmock.a lib/libgmock_main.a /usr/lib/
```

或用 FetchContent (推荐, 不污染系统):

#### 3.7.2 CMakeLists.txt 加测试开关

在 `CMakeLists.txt` 末尾追加:

```cmake
option(ENABLE_TESTING "Build unit tests" OFF)

if(ENABLE_TESTING)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    FetchContent_MakeAvailable(googletest)

    enable_testing()
    add_subdirectory(test)
endif()
```

#### 3.7.3 创建测试目录

```
<project_root>/
└── test/
    ├── CMakeLists.txt
    ├── test_pack_codec.cpp     # 测 protocol/pack_codec
    ├── test_game_server.cpp    # 测 server/handlers
    └── test_ecs.cpp            # 测 server/ecs
```

`test/CMakeLists.txt`:
```cmake
# 例: 测 pack_codec
add_executable(test_pack_codec test_pack_codec.cpp)
target_link_libraries(test_pack_cuda PRIVATE gtest gtest_main)
target_link_libraries(test_pack_codec PRIVATE
    $<TARGET_OBJECTS:game_proto>      # 复用工程 OBJECT 库
)
target_include_directories(test_pack_codec PRIVATE ${INC_DIRS})

include(GoogleTest)
gtest_discover_tests(test_pack_codec)
```

`test/test_pack_codec.cpp` 示例:
```cpp
#include <gtest/gtest.h>
#include "protocol/pack_codec.h"
#include "protocol/pack_flags.h"

using PackFrame;
using EncodeFrame;
using TryDecodeFrames;

TEST(PackCodec, EncodeDecodeRoundTrip) {
    PackFrame in{.msg_id = 0x12345678, .flags = 0x00FF, .body = "hello"};
    auto buf = EncodeFrame(in);

    std::vector<PackFrame> outs;
    ASSERT_TRUE(TryDecodeFrames(buf, &outs));
    ASSERT_EQ(outs.size(), 1u);
    EXPECT_EQ(outs[0].msg_id, in.msg_id);
    EXPECT_EQ(outs[0].flags, in.flags);
    EXPECT_EQ(outs[0].body, in.body);
}

TEST(PackCodec, DecodeTruncatedFails) {
    std::string buf = "abc";   // 太短
    std::vector<PackFrame> outs;
    EXPECT_FALSE(TryDecodeFrames(buf, &outs));
}
```

#### 3.7.4 构建并运行测试

```bash
# 构建 (默认不开启测试)
cmake .
make -j$(nproc)

# 开启测试
cmake . -DENABLE_TESTING=ON
make -j$(nproc)

# 跑全部测试
ctest --output-on-failure

# 或直接跑单个测试二进制
./test/test_pack_codec

# gtest 过滤
./test/test_pack_codec --gtest_filter=PackCodec.*
./test/test_pack_codec --gtest_filter=-*Truncated*   # 排除
```

### 3.8 zrpc 自带测试

zrpc 在 `third_party/zrpc/http/test/http_regression_test.cc` 有一个 HTTP 回归测试 (但 `CMakeLists.txt` 默认排除了 `/test/`)。若要启用:

```cmake
# 在 CMakeLists.txt 第 90 行附近修改排除规则, 不要排除 /test/
# list(FILTER ZRPC_SRC EXCLUDE REGEX "/test/")   # 注释掉
```

但注意它的 `main()` 与 `main.cc` 冲突, 需要单独编一个目标:

```cmake
if(ENABLE_TESTING)
    add_executable(zrpc_http_regression_test
        "${ZRPC_DIR}/http/test/http_regression_test.cc"
        $<TARGET_OBJECTS:zrpc>
    )
    target_include_directories(zrpc_http_regression_test PRIVATE ${INC_DIRS})
    target_link_libraries(zrpc_http_regression_test PRIVATE pthread)
endif()
```

---

## 四、常见问题排查

### 4.1 `error while loading shared libraries: libprotobufd.so`

运行时找不到 `.so`:

```bash
# 检查 RPATH
readelf -d ./bin/svc_game_3d_server | grep -i rpath
# 应看到: RPATH: $ORIGIN

# 检查 bin/ 下是否有 .so
ls bin/libprotobufd.so*

# 若缺失, 手动拷贝
cp lib/libprotobufd.so* bin/

# 临时方案: 设 LD_LIBRARY_PATH
export LD_LIBRARY_PATH=<project_root>/bin:$LD_LIBRARY_PATH
```

### 4.2 `undefined reference to ...` (链接错误)

通常是漏编了某个 .cpp。检查 `CMakeLists.txt` 第 113-146 行 `game_proto` 和 `svc_game_3d_server` 的源文件列表是否包含报错符号所在文件。

### 4.3 编译报 `xxx.pb.h: No such file or directory`

proto 生成代码缺失, 跑:
```bash
./build.sh proto
make -j$(nproc)
```

### 4.4 改了 CMakeLists.txt 不生效

`make` 会自动检测 `CMakeLists.txt` 变化并重跑 `cmake`, 但若 cache 变量被污染, 用:
```bash
./build.sh reconfigure
# 或
rm CMakeCache.txt && cmake .
```

### 4.5 SIGPIPE 导致 server 崩溃

客户端断连后 server 写 socket 触发 SIGPIPE。`server/main.cc` 已加 `signal(SIGPIPE, SIG_IGN)`, 若仍崩溃检查:
```bash
# 确认进程忽略 SIGPIPE
cat /proc/$(pgrep svc_game_3d_server)/status | grep Sig
# 应在 SigIgn 中看到 0x10 (第 14 位, SIGPIPE=13)
```

### 4.6 WSL2 时间不准 (导致心跳超时判断错误)

WSL2 时钟可能漂移, 影响定时器:
```bash
sudo hwclock -s          # 同步硬件时钟
# 或重启 WSL (PowerShell)
wsl --shutdown
```

---

## 五、目录结构速查

```
<project_root>/
├── CMakeLists.txt           # 主构建文件 (in-source build)
├── build.sh                 # WSL 构建封装脚本
├── README.md                # 本文档
├── doc\                     # 分模块设计/流程文档（服务端见 doc/server/）
├── .gitignore               # 忽略 CMake 产物
│
├── src\                     # 源码
│   ├── client\              #   client / stress_test / bench / pingpong / server.cc
│   ├── common\              #   init.h
│   ├── protocol\            #   pack_codec.{cpp,h} / pack_flags.h
│   └── server\              #   game_server / ecs / handlers / systems
│       # 流程文档已迁至 ../../doc/server/（勿再在本目录堆单体 MD）
│
├── third_party\             # 第三方库
│   ├── zrpc\                #   网络库 (base / net / http, 排除 rpc/grpc/example/test)
│   └── joltphysics\         #   物理引擎
│
├── deps\
│   └── protobuf\            # protobuf 头文件 (include/google/protobuf/*)
│
├── protobuf\                # proto 工具 + 原始 .proto + 生成代码
│   ├── protoc.exe           #   Windows protoc
│   ├── gen.bat               #   一键生成 .pb.cc/.pb.h + lua + Msg_*.lua
│   ├── proto_gen.bat         #   原始工具 (调 dist/generate.exe + modify.exe)
│   ├── proto_gen_lua.bat     #   lua 生成
│   ├── dist\                 #   generate.exe / modify.exe / generate_lua.exe
│   ├── script\               #   Python 脚本源码
│   └── protos\               #   原始 .proto (18 个) + 生成产物
│       ├── c++\              #     .pb.cc / .pb.h / .auto.cpp (CMake 直接引用)
│       ├── lua\              #     .pb (Lua 描述文件)
│       ├── Msg_Svr_protobuf.lua  # 服务端消息 ID 注册表 (CRC32)
│       └── Msg_Cli_protobuf.lua  # 客户端消息 ID 注册表
│
├── lib\                     # libprotobufd.so* (链接用)
│
└── bin\                     # 编译产物输出 (自动拷贝 .so 到此)
```
