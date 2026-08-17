# MongoDB Windows 服务安装与开机自启配置

> 记录在 Windows 上将 MongoDB 8.x 注册为系统服务、配置开机自启与崩溃自动重启的完整流程。
> 包含实际踩过的坑和最终可用的脚本。

---

## 一、环境信息

| 项目 | 值 |
|---|---|
| 操作系统 | Windows 11 64-bit（远程内网机） |
| 物理内存 | 32.0 GB |
| MongoDB 版本 | 8.3.7 |
| 安装方式 | ZIP 解压版（非 MSI） |
| mongod.exe 路径 | `C:\mongo\mongodb-win32-x86_64-windows-8.3.7\bin\mongod.exe` |
| 数据目录 | `C:\mongo\data\db` |
| 日志文件 | `C:\mongo\log\mongod.log` |
| 配置文件 | `C:\mongo\mongodb-win32-x86_64-windows-8.3.7\bin\mongod.cfg` |
| 监听端口 | 27017 |
| 绑定地址 | 0.0.0.0（允许远程访问） |

---

## 二、背景与问题

### 原始问题
MongoDB 之前是手动运行 `mongod.exe`，存在以下问题：
1. 关闭终端 / 重启机器后 mongo 自动停止
2. 进程崩溃后没有自动重启机制
3. 出现"过一段时间就不监听 27017"的现象

### 根因
**MongoDB 没有注册为 Windows 服务**，手动运行的 mongod 在终端关闭、机器重启或进程崩溃后会停止，且无法自动恢复。

### 解决方案
将 MongoDB 注册为 Windows 服务，配置为：
- 开机自动启动（`StartupType = Automatic`）
- 崩溃后自动重启（通过 `services.msc` 恢复策略配置）

---

## 三、踩过的坑（重要！）

### 坑 1：`mongod.exe` 路径不存在
**现象**：`.\mongod.exe` 报 `CommandNotFoundException`

**原因**：假设的安装路径 `C:\Program Files\MongoDB\Server\7.0\bin\` 不存在，实际是 ZIP 解压到 `C:\mongo\mongodb-win32-x86_64-windows-8.3.7\bin\`

**解决**：用 `Get-ChildItem -Recurse -Filter "mongod.exe"` 全盘搜索定位真实路径。

### 坑 2：配置文件路径必须是绝对路径
**现象**：`--config ".\mongod.cfg"` 报错
```
"errmsg":"config requires an absolute file path with Windows services"
```

**原因**：Windows 服务模式下，mongod 的 `--config` 参数不接受相对路径。

**解决**：必须用绝对路径：
```powershell
--config "C:\mongo\mongodb-win32-x86_64-windows-8.3.7\bin\mongod.cfg"
```

### 坑 3：`--install` 必须配合 `--logpath`
**现象**：只用 `--config` 报错
```
"msg":"--install has to be used with a log file for server output"
```

**原因**：MongoDB 8.x 在 `--install` 阶段对配置文件的日志解析比较苛刻。

**解决**：命令行同时指定 `--config`、`--logpath`、`--dbpath` 三重保险：
```powershell
--config "$cfgPath" --logpath "$logPath" --dbpath "$dbPath" --install
```

### 坑 4：MongoDB 8.x 已移除 `storage.journal.enabled`
**现象**：注册服务报错
```
Unrecognized option: storage.journal.enabled
```

**原因**：从 MongoDB 4.0 起 journal 强制开启且不可关闭，**8.x 彻底删除了 `storage.journal` 配置段**，写了就直接报错退出。

**解决**：配置文件中**不能出现**以下选项：
- `storage.journal.enabled`
- `storage.journal.commitIntervalMs`
- `storage.journal`（整段）

### 坑 5：配置文件编码问题
**现象**：`Out-File -Encoding ASCII` 写出的文件可能带 BOM 或格式异常，导致 mongod 解析失败。

**解决**：用 .NET 方法写入，确保纯 ASCII 无 BOM：
```powershell
[System.IO.File]::WriteAllText($cfgPath, $cfgContent, [System.Text.Encoding]::ASCII)
```

### 坑 6：PowerShell 多行 if-else 粘贴问题
**现象**：复制多行脚本到 PowerShell 时，`else` 单独成行报 `CommandNotFoundException`。

**原因**：PowerShell 交互式模式下，多行 `if { } else { }` 被拆开粘贴会导致语法解析中断。

**解决**：把完整脚本保存为 `.ps1` 文件执行，或确保 `else` 紧跟在 `if` 块的 `}` 同一行/连续粘贴。

---

## 四、最终可用配置文件

**路径**：`C:\mongo\mongodb-win32-x86_64-windows-8.3.7\bin\mongod.cfg`

```yaml
storage:
  dbPath: C:\mongo\data\db
  wiredTiger:
    engineConfig:
      cacheSizeGB: 6

systemLog:
  destination: file
  path: C:\mongo\log\mongod.log
  logAppend: true

net:
  port: 27017
  bindIp: 0.0.0.0
```

### 配置说明

| 配置项 | 值 | 说明 |
|---|---|---|
| `storage.dbPath` | `C:\mongo\data\db` | 数据存储目录，必须提前创建 |
| `storage.wiredTiger.engineConfig.cacheSizeGB` | `6` | WiredTiger 缓存大小，32GB 内存机器推荐 6GB |
| `systemLog.path` | `C:\mongo\log\mongod.log` | 日志文件路径 |
| `systemLog.logAppend` | `true` | 日志追加模式，重启不覆盖 |
| `net.port` | `27017` | MongoDB 默认端口 |
| `net.bindIp` | `0.0.0.0` | 允许远程访问；仅本机用 `127.0.0.1` |

### cacheSizeGB 调整规则（按机器内存）

| 物理内存 | 推荐 cacheSizeGB |
|---|---|
| 2 GB | 0.5 |
| 4 GB | 1.5 |
| 8 GB | 3 |
| 16 GB | 6 |
| **32 GB**（当前） | **6** |
| 64 GB+ | 16 或不设（用默认值） |

---

## 五、最终可用的安装脚本（一键执行）

> **必须用管理员身份打开 PowerShell**（右键开始菜单 → 终端(管理员)）

```powershell
# ============ 1. 变量 ============
$mongoBin = "C:\mongo\mongodb-win32-x86_64-windows-8.3.7\bin"
$cfgPath  = "$mongoBin\mongod.cfg"
$dbPath   = "C:\mongo\data\db"
$logPath  = "C:\mongo\log\mongod.log"

# ============ 2. 杀残留 + 清理旧服务 ============
Get-Process mongod -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2
& "$mongoBin\mongod.exe" --remove --serviceName "MongoDB" 2>&1 | Out-Null

# ============ 3. 确保目录存在 ============
New-Item -ItemType Directory -Force -Path $dbPath | Out-Null
New-Item -ItemType Directory -Force -Path (Split-Path $logPath) | Out-Null

# ============ 4. 写入极简配置（注意：8.x 不能有 storage.journal） ============
$cfgContent = @"
storage:
  dbPath: $dbPath
  wiredTiger:
    engineConfig:
      cacheSizeGB: 6

systemLog:
  destination: file
  path: $logPath
  logAppend: true

net:
  port: 27017
  bindIp: 0.0.0.0
"@
[System.IO.File]::WriteAllText($cfgPath, $cfgContent, [System.Text.Encoding]::ASCII)

Write-Host "==== 配置文件内容 ====" -ForegroundColor Cyan
Get-Content $cfgPath

# ============ 5. 注册服务（cfg + logpath + dbpath 三重保险） ============
Write-Host "`n==== 注册服务 ====" -ForegroundColor Cyan
& "$mongoBin\mongod.exe" `
    --config "$cfgPath" `
    --logpath "$logPath" `
    --dbpath "$dbPath" `
    --install `
    --serviceName "MongoDB" `
    --serviceDisplayName "MongoDB"

# ============ 6. 检查注册结果 ============
Start-Sleep -Seconds 2
$svc = Get-Service MongoDB -ErrorAction SilentlyContinue

if ($svc) {
    Write-Host "`n✅ 服务注册成功，状态: $($svc.Status)" -ForegroundColor Green

    # 设为自动启动
    Set-Service -Name "MongoDB" -StartupType Automatic

    # 启动服务
    Start-Service MongoDB
    Start-Sleep -Seconds 3

    Write-Host "`n==== 服务状态 ====" -ForegroundColor Green
    Get-Service MongoDB | Format-Table Name, Status, StartType -AutoSize

    Write-Host "`n==== 端口监听 ====" -ForegroundColor Green
    $port = netstat -ano | findstr :27017
    if ($port) {
        $port
        Write-Host "`n✅✅✅ MongoDB 启动成功，正在监听 27017" -ForegroundColor Green
    } else {
        Write-Host "`n❌ 端口未监听" -ForegroundColor Red
    }

    Write-Host "`n==== 最新日志 ====" -ForegroundColor Green
    Get-Content $logPath -Tail 15
} else {
    Write-Host "`n❌ 服务注册失败" -ForegroundColor Red
    Get-Content $logPath -Tail 30 -ErrorAction SilentlyContinue
}
```

---

## 六、成功验证标志

执行脚本后，看到以下输出即为成功：

```
==== 服务状态 ====
Name     Status StartType
----     ------ ---------
MongoDB Running Automatic

==== 端口监听 ====
  TCP    0.0.0.0:27017    0.0.0.0:0    LISTENING    16760

✅✅✅ MongoDB 启动成功，正在监听 27017
```

日志关键行：
```json
{"msg":"Listening on","attr":{"address":"0.0.0.0:27017"}}
{"msg":"mongod startup complete"}
```

> 注意：`address` 必须是 `0.0.0.0:27017`（允许远程），如果是 `127.0.0.1:27017` 说明配置文件没生效。

---

## 七、后续配置（强烈建议完成）

### 1. 配置崩溃自动重启（关键！）

1. `Win + R` → 输入 `services.msc` → 回车
2. 找到 **MongoDB** → 双击
3. 切到【恢复】标签页，按下面配置：

| 选项 | 设置 |
|---|---|
| 第一次失败 | 重新启动服务 |
| 第二次失败 | 重新启动服务 |
| 后续失败 | 重新启动服务 |
| 在此后重新启动服务 | 5 分钟 |
| 在此后重置失败计数 | 1 天 |

4. 点【确定】

这样即使 mongod 崩溃，Windows 也会自动把它拉起来。

### 2. 把数据目录加入 Defender 排除（防崩溃）

用管理员 PowerShell 执行：

```powershell
Add-MpPreference -ExclusionPath "C:\mongo\data\db"
Add-MpPreference -ExclusionPath "C:\mongo\log"
Add-MpPreference -ExclusionProcess "C:\mongo\mongodb-win32-x86_64-windows-8.3.7\bin\mongod.exe"
```

> Windows Defender 实时扫描 `C:\mongo\data\db` 下的文件可能导致 mongod 崩溃，必须排除。

### 3. 验证开机自启

**重启机器**，重启后执行：

```powershell
Get-Service MongoDB              # 应显示 Running
netstat -ano | findstr :27017    # 应有 LISTENING 输出
```

如果重启后 mongo 自动起来，说明开机自启配置成功。

---

## 八、常用运维命令

### 服务管理

```powershell
# 查看服务状态
Get-Service MongoDB

# 启动 / 停止 / 重启
Start-Service MongoDB
Stop-Service MongoDB
Restart-Service MongoDB

# 修改启动类型
Set-Service -Name "MongoDB" -StartupType Automatic   # 自动
Set-Service -Name "MongoDB" -StartupType Manual      # 手动
Set-Service -Name "MongoDB" -StartupType Disabled    # 禁用
```

### 卸载服务

```powershell
# 停止服务
Stop-Service MongoDB

# 卸载
& "C:\mongo\mongodb-win32-x86_64-windows-8.3.7\bin\mongod.exe" --remove --serviceName "MongoDB"
```

### 修改配置后生效

```powershell
# 修改 mongod.cfg 后，重启服务让配置生效
Restart-Service MongoDB

# 验证日志
Get-Content C:\mongo\log\mongod.log -Tail 30
```

### 查看实时日志

```powershell
# 实时跟踪日志输出（类似 Linux tail -f）
Get-Content C:\mongo\log\mongod.log -Wait -Tail 50
```

### 连接测试

```powershell
# 用 mongosh 连接（如果装了）
& "C:\mongo\mongodb-win32-x86_64-windows-8.3.7\bin\mongosh.exe" "mongodb://127.0.0.1:27017"

# 或用 MongoDB Compass 连接
# 连接字符串：mongodb://127.0.0.1:27017
```

---

## 九、MongoDB 8.x 配置变更备忘

以下选项在 MongoDB 8.x 中**已移除或变更**，配置文件中不能出现：

| 选项 | 状态 | 说明 |
|---|---|---|
| `storage.journal.enabled` | **移除** | journal 强制开启，不可关闭 |
| `storage.journal.commitIntervalMs` | **移除** | 同上 |
| `storage.journal`（整段） | **移除** | 同上 |
| `storage.engine` | 移除 | 只支持 wiredTiger |
| `storage.mmapv1.*` | 移除 | mmapv1 引擎已删除 |
| `replication.enableMajorityReadConcern` | 移除 | 强制开启 |

8.x 推荐保留的核心配置段：
- `storage.dbPath`
- `storage.wiredTiger.engineConfig.cacheSizeGB`
- `systemLog.destination` / `path` / `logAppend`
- `net.port` / `net.bindIp`
- `security.authorization`（启用认证时）
- `operationProfiling`（按需）

---

## 十、故障排查

### 服务启动失败

```powershell
# 1. 看服务状态
Get-Service MongoDB

# 2. 看日志
Get-Content C:\mongo\log\mongod.log -Tail 50

# 3. 看 Windows 事件日志
Get-EventLog -LogName Application -Newest 20 -EntryType Error |
    Where-Object { $_.Message -like "*mongod*" -or $_.Source -like "*MongoDB*" } |
    Format-List TimeGenerated, Source, Message
```

### 端口被占用

```powershell
# 查看 27017 端口被哪个进程占用
netstat -ano | findstr :27017

# 根据 PID 查进程名
Get-Process -Id <PID>
```

### 数据目录权限问题

```powershell
# 查看权限
icacls C:\mongo\data\db

# 确保 SYSTEM 和 Administrators 有完全控制
```

### 磁盘空间不足

```powershell
# 查看 C 盘剩余空间
Get-PSDrive C | Select-Object Used, Free
```

> MongoDB 默认在磁盘剩余 < 5% 时会停止写入或退出。

---

## 十一、相关文档

- [mongo_account_info_id_design.md](./mongo_account_info_id_design.md) - 账号信息 ID 设计
- [mongo_changes.md](./mongo_changes.md) - MongoDB 变更记录
- [mongo_persist_thread_safety_fix.md](./mongo_persist_thread_safety_fix.md) - 持久化线程安全修复
- [persist_auto_dirty_design.md](./persist_auto_dirty_design.md) - 自动脏数据标记设计

---

## 变更记录

| 日期 | 变更 |
|---|---|
| 2026-08-10 | 初始版本，记录 MongoDB 8.3.7 在 Windows 11 上的服务安装过程 |
