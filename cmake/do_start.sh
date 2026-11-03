#!/bin/bash

# 获取程序路径
curr="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
svc_name="$1"
# 自适应 exe_dir: 若脚本在 release/ 目录下(存在 bin 子目录)则用 $curr/bin, 否则回退到 $curr/release/bin
if [ -d "$curr/bin" ]; then
    exe_dir="$curr/bin"
else
    exe_dir="$curr/release/bin"
fi
exe_name="$exe_dir/$svc_name"

# 使用圆角字形和 ANSI 颜色渲染日志级别标签。
colorize_log_level() {
    sed -u \
        -e $'s/\\[TRACE\\]/ \033[36m\033[30;46m TRACE \033[0;36m\033[0m /g' \
        -e $'s/\\[DEBUG\\]/ \033[36m\033[30;46m DEBUG \033[0;36m\033[0m /g' \
        -e $'s/\\[INFO\\]/ \033[37m\033[30;47m INFO \033[0;37m\033[0m /g' \
        -e $'s/\\[WARN\\]/ \033[33m\033[30;43m WARN \033[0;33m\033[0m /g' \
        -e $'s/\\[ERROR\\]/ \033[31m\033[37;41m ERROR \033[0;31m\033[0m /g' \
        -e $'s/\\[FATAL\\]/ \033[35m\033[37;45m FATAL \033[0;35m\033[0m /g'
}

set_nofile_limit() {
    # 设置指定PID的nofile软硬限制为65535（需要sudo）
    local target_pid="$1"
    if command -v prlimit >/dev/null 2>&1; then
        sudo prlimit --pid "$target_pid" --nofile=65535:65535
    else
        echo "未找到 prlimit，跳过设置 nofile 限制"
    fi
}

if [ -z "$svc_name" ]; then
    echo "用法: $0 <svc_name>"
    exit 1
fi

if [ ! -x "$exe_name" ]; then
    echo "可执行文件不存在或不可执行: $exe_name"
    exit 1
fi

echo "设置 core dump ..."
ulimit -c unlimited

server_name="$(basename "$curr")"
CORE_BASE_DIR="/data/dump"
CORE_DIR="$CORE_BASE_DIR/$server_name"

sudo mkdir -p "$CORE_DIR"
sudo chmod 777 "$CORE_DIR"

if echo "$CORE_DIR/core.%e.%p.%t" | sudo tee /proc/sys/kernel/core_pattern >/dev/null; then
    echo "已设置 core_pattern=$CORE_DIR/core.%e.%p.%t"
    echo "core 文件将生成到: $CORE_DIR"
else
    echo "WARNING: 设置 /proc/sys/kernel/core_pattern 失败"
fi

# 找到进程的PID
echo "Target executable: $exe_name"
# 使用正则边界匹配，防止 svc_log 匹配到 svc_login
# \b 匹配单词边界，或者使用 ($|\s) 匹配行尾或空格
pid=$(pgrep -f "${exe_name}\b")

# 检查是否找到进程
if [ -n "$pid" ]; then
    echo "Found existing process PIDs: $pid"
    echo "Killing process..."
    # 注意：这里不能加引号，否则如果有多个PID（包含换行符），kill会报错导致无法杀死进程
    kill -9 $pid
    echo "Process killed."
fi

# 启动程序
# 关键：切到区服根目录
cd "$exe_dir" || exit 1

# ---------- ASan 日志落盘 ----------
# 默认 ASan 报告输出到 stderr，被 stdout 合并后会污染应用日志且无法独立查看。
# 这里通过 ASAN_OPTIONS=log_path 让 ASan 把报告单独写到 /tmp/asan/asan.<pid>。
ASAN_LOG_DIR="/tmp/asan"
mkdir -p "$ASAN_LOG_DIR"
chmod 777 "$ASAN_LOG_DIR" 2>/dev/null || true
echo "创建并设置 asan 目录: $ASAN_LOG_DIR"
export ASAN_OPTIONS="detect_leaks=1:abort_on_error=1:halt_on_error=1:fast_unwind_on_malloc=0:malloc_context_size=30:detect_stack_use_after_return=1:log_path=$ASAN_LOG_DIR/asan:print_legend=1:print_stats=1"
export LSAN_OPTIONS="report_objects=1:log_threads=1"
# 可选：符号化（如果装了 llvm-symbolizer）
export ASAN_SYMBOLIZER_PATH=$(which llvm-symbolizer-14 2>/dev/null || which llvm-symbolizer 2>/dev/null || true)

# 如果不需要彩色日志，去掉 colorize_log_level 部分
# 设置动态库搜索路径，确保能找到 bin 目录下的 .so 文件
export LD_LIBRARY_PATH="$exe_dir:$LD_LIBRARY_PATH"

# 设置 MariaDB 插件目录，确保能找到 bin/plugin 下的认证插件（如 caching_sha2_password.so）
export MARIADB_PLUGIN_DIR="$exe_dir/plugin"
export MYSQL_PLUGIN_DIR="$exe_dir/plugin"

"$exe_name" > >(colorize_log_level) 2>&1 &

echo -ne "\e]0;$1\e\\"

PID=$!

set_nofile_limit "$PID"

# 设置陷阱，当接收到 SIGINT 或 SIGTERM 信号时，杀死程序并退出
trap "kill $PID 2>/dev/null; exit" SIGINT SIGTERM

# 等待程序结束
wait $PID

# 程序结束后，退出 WSL
exit
