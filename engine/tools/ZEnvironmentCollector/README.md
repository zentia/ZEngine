# Windows Environment Check

## Launcher 用法

```powershell
ZEnvironmentCollector.exe -batchmode --preflight [--game-dir <游戏目录>] [--game-exe <exe名>] [--min-spec <min-spec.json路径>] [--budget-seconds N] [--output-dir <目录>] [--quiet]
```

`-batchmode` 沿用 Unity/UE 的写法：不开窗口，结论写进输出目录、进度与错误写 stdout，退出码交给调用方。**一次只允许一个实例**，第二个直接退出（`31`）。默认最多 3 秒（`--budget-seconds` 1–30），到点按 `Inconclusive` 结束。只读取机器状态：不扫盘、不加载游戏 DLL、不写注册表、不提权。

### 退出码

| 退出码 | 含义 |
|---:|---|
| `0` | 结论已写入 `OSGamePreflight.json` |
| `30` | 工具内部错误 |
| `31` | 已有实例在运行，本次未执行 |

### 结论

`OSGamePreflight.json` 里 launcher 只需四个键：`verdict` / `confidence` / `userMessage` / `actions`。诊断报告共用同一套键，`"mode":"preflight"` 用来区分；`findings` 是同一批结论的明细，每项带 `code` / `layer` / `zh` / `evidence`。

**提示由工具给，不由 launcher 写**：`userMessage` 与 `actions` 的每一项都是 `{"code", "zh"}`，`zh` 就是给玩家看的那句话，`code` 是它的稳定代号——按 `code` 去重和埋点，不要比较文案。工具从不阻断启动，超时也只是 `Inconclusive`。

| verdict | 含义 | 工具给出的 userMessage |
|---|---|---|
| `Pass` | 没发现值得说的 | 「未发现明显的环境问题，可以正常启动游戏。」（`NO_ISSUES_FOUND`） |
| `Advise` | 可提示的环境风险（驱动版本不一致、选到集显、页面文件过小、缺 VC++ 运行库、空间不足、等重启、装在机械硬盘、路径含非 ASCII…） | 那一项的说明；有多项时为「检测到 N 项可能影响游戏体验的环境问题：…」（`ENVIRONMENT_ADVICE`） |
| `BelowMinimum` | 缺 AVX2、内存低于最低要求、非 64 位 Windows、没有显卡能初始化 D3D11、功能级别低于 `minimumFeatureLevel` | 那条硬要求的说明，`code` 即该 finding 的 code |
| `Inconclusive` | 没跑完（超时或探针失败） | 「本次环境检查没有跑完，未得出结论。可以直接启动游戏。」（`PREFLIGHT_INCOMPLETE`） |

`actions` 只列工具认为玩家能自己做的那几步（如 `INSTALL_VC_REDIST` 装 VC++ 运行库、`ENABLE_SYSTEM_MANAGED_PAGE_FILE` 把页面文件交回系统管理、`UPGRADE_OS` 换 64 位系统），按 code 去重，兜底是 `CONTACT_SUPPORT` 提交环境报告。原样展示 `zh` 即可。

### 不兼容程序（ProcMon）检测

`--preflight` 还会做一次只读扫描：进程名快照（`CreateToolhelp32Snapshot`）加活动驱动服务枚举（`EnumServicesStatusExW`），用来发现游戏拒绝与之同时运行的第三方程序。目前名单覆盖 Sysinternals Process Monitor：`procmon.exe` / `procmon64.exe` / `procmon64a.exe`，以及它注册为 `PROCMONxx`（如 `PROCMON23`）的内核驱动服务——关掉窗口但驱动还留着、或把 exe 改名的情况也能查到。命中时：

- 新增一条 `INCOMPATIBLE_PROGRAM_DETECTED` finding，`zh` 与 `evidence` 里列出具体名字；
- verdict 变成 `Advise`，`userMessage` 提示关闭程序后重启游戏，`actions` 给出 `CLOSE_INCOMPATIBLE_PROGRAM`；
- **报告只是提示，工具不阻断启动**。游戏里那句 "Incompatible program ProcMon detected, please close it and restart" 是游戏自己的兼容性检查（反作弊组件）弹的，它不读这个 JSON，也不归本工具管；两处名单要同步改，否则又会出现"报告说没问题、游戏却拒绝启动"。

`environment.incompatibleProgramScan` 段记录 `processSnapshotKnown` / `driverServicesKnown` / `detected`。扫描没跑完时对应 known 为 `false`，此时不产生 finding（fail-open），并往 `notes` 里写一条说明——**Pass 不等于"查过程序且没发现"**。名单在 `Preflight.cpp` 的 `IncompatibleProcessNames` 与 `IsIncompatibleDriverServiceName()`，改动时同步更新下面的表格和 `PreflightTests.cpp`。

### code 对照表

**findings[].code（BelowMinimum，硬要求，工具认为游戏跑不起来）**

| code | layer | actions[].code |
|---|---|---|
| `WINDOWS_X64_REQUIRED` | system | `UPGRADE_OS` |
| `CPU_ISA_UNSUPPORTED` | cpu | `CONTACT_SUPPORT`（兜底） |
| `PHYSICAL_MEMORY_INSUFFICIENT` | memory | `FREE_MEMORY` |
| `D3D11_DEVICE_UNAVAILABLE` | gpu | `INSTALL_GPU_DRIVER` |
| `D3D11_ONLY_SOFTWARE_ADAPTER` | gpu | `INSTALL_GPU_DRIVER` |
| `D3D_FEATURE_LEVEL_BELOW_MINIMUM` | gpu | `UPGRADE_GPU` |

**findings[].code（Advise，只提示、不阻断）**

| code | layer | actions[].code |
|---|---|---|
| `D3D11_NOT_ON_PREFERRED_ADAPTER` | gpu | `SELECT_DISCRETE_GPU` |
| `GPU_DRIVER_MIXED` | gpu | `UPDATE_GPU_DRIVER` |
| `GPU_SOFTWARE_RENDERER` | gpu | `INSTALL_GPU_DRIVER` |
| `GPU_INTEGRATED_SELECTED` | gpu | `SELECT_DISCRETE_GPU` |
| `GPU_VIDEO_MEMORY_BELOW_MINIMUM` | gpu | `LOWER_GAME_SETTINGS` |
| `MONITOR_ON_INTEGRATED_GPU` | gpu | `PLUG_MONITOR_INTO_DISCRETE_GPU` |
| `GPU_DRIVER_RESET_RECENT` | gpu | `INSTALL_GPU_DRIVER` |
| `MEMORY_AVAILABLE_LOW` | memory | `CLOSE_BACKGROUND_APPS` |
| `PAGE_FILE_TOO_SMALL` | memory | `ENABLE_SYSTEM_MANAGED_PAGE_FILE` |
| `VC_RUNTIME_MISSING` | runtime | `INSTALL_VC_REDIST` |
| `STORAGE_LOW_FREE_SPACE` | storage | `FREE_DISK_SPACE` |
| `SYSTEM_VOLUME_LOW_FREE_SPACE` | storage | `FREE_SYSTEM_DISK_SPACE` |
| `GAME_VOLUME_REMOVABLE_OR_REMOTE` | storage | `REINSTALL_ON_LOCAL_DISK` |
| `GAME_VOLUME_NOT_NTFS` | storage | `REINSTALL_ON_LOCAL_DISK` |
| `GAME_VOLUME_SPINNING_DISK` | storage | `REINSTALL_ON_SSD` |
| `GAME_PATH_NON_ASCII` | storage | `REINSTALL_TO_ASCII_PATH` |
| `WINDOWS_INSIDER_PREVIEW` | system | `LEAVE_INSIDER` |
| `REBOOT_PENDING` | system | `RESTART_COMPUTER` |
| `UPTIME_VERY_LONG` | system | `RESTART_COMPUTER` |
| `INCOMPATIBLE_PROGRAM_DETECTED` | system | `CLOSE_INCOMPATIBLE_PROGRAM` |

**actions[].code（玩家能自己做的那一步，按 code 去重）**：共 20 个，含义列只是给读文档的人看的概要，实际展示用报告里的 `zh`（文案会调，以 code 为准）；`CONTACT_SUPPORT` 是兜底，保证 `actions` 至少有一项。

| actions[].code | 含义 |
|---|---|
| `UPDATE_GPU_DRIVER` | 把两张显卡的驱动更新到同一版本 |
| `SELECT_DISCRETE_GPU` | 在显卡面板里指定游戏用独立显卡 |
| `INSTALL_GPU_DRIVER` | 装显卡官方驱动并重启 |
| `LOWER_GAME_SETTINGS` | 调低画质/分辨率（显存不足） |
| `PLUG_MONITOR_INTO_DISCRETE_GPU` | 显示器改接独立显卡输出口 |
| `FREE_DISK_SPACE` | 清理游戏所在盘，至少留 2 GB |
| `FREE_SYSTEM_DISK_SPACE` | 清理系统盘，至少留 2 GB |
| `UPGRADE_GPU` | 显卡功能级别不够，换显卡或升级驱动 |
| `RESTART_COMPUTER` | 重启电脑（待重启更新/开机过久） |
| `REINSTALL_ON_LOCAL_DISK` | 重装到本地固定磁盘的 NTFS 分区 |
| `REINSTALL_TO_ASCII_PATH` | 重装到纯英文数字路径 |
| `REINSTALL_ON_SSD` | 把游戏挪到 SSD |
| `LEAVE_INSIDER` | 退出 Windows 预览体验计划 |
| `FREE_MEMORY` | 关掉占内存的程序或加内存 |
| `CLOSE_BACKGROUND_APPS` | 关掉浏览器/录屏/下载等占内存的程序 |
| `ENABLE_SYSTEM_MANAGED_PAGE_FILE` | 页面文件交回系统管理 |
| `INSTALL_VC_REDIST` | 装 VC++ 2015-2022 运行库（x64）并重启 |
| `UPGRADE_OS` | 换 64 位 Windows |
| `CLOSE_INCOMPATIBLE_PROGRAM` | 关闭 Process Monitor 等不兼容程序并重启游戏 |
| `CONTACT_SUPPORT` | 提交环境报告联系客服（兜底） |

launcher 不等工具跑完，而是边跑边轮询输出目录，所以报告文件**先写临时文件、再 `MoveFileEx` 改名发布**：读者只会看到"旧的完整文件"或"新的完整文件"。

- **读报告请用 `FILE_SHARE_DELETE` 打开，读完立刻关闭**：MSVC 的 `std::ifstream` 不共享删除，一直开着不放会让发布失败。工具重试约 1 秒后放弃，等价于"本次没有产出报告"——按"没检查过"处理（fail-open）。
- 每次运行重写同名报告。默认输出目录 `%LOCALAPPDATA%\OSGame\EnvCheck`（不是当前目录：launcher 拉起时当前目录是游戏安装目录）。
- 工具自身日志在 `%USERPROFILE%\AppData\LocalLow\ZEngine\Logs\ZEnvironmentCollector_YYYYMMDD_N.log`。字体缺失、D3D 初始化失败、后台任务报错只在那里看得到。

### 最低配置 min-spec.json

最低配置是**游戏**的属性：随游戏发布、每次拉起时读取（默认 `<游戏目录>\min-spec.json`，`--min-spec <path>` 覆盖）。扁平对象，值是布尔或整数，唯一例外是 `minimumFeatureLevel` 写 `"11_0"` 这类名称；没写的键用内置默认，**`0` 表示关掉这条规则**；文件不存在或写坏了回滚到内置默认继续跑。报告另有 `"requirements"`（本次判定用的全部阈值）与 `"requirementsSource"`（`default` 或 `file:<路径>`）。

```json
{
  "avx": false, "avx2": true, "fma": false, "f16c": false,
  "minimumMemoryBytes": 4294967296,
  "minimumAvailableMemoryBytes": 2147483648,
  "minimumPageFileBytes": 2147483648,
  "pageFileRuleMaxMemoryBytes": 17179869184,
  "minimumPageFilePercentOfRam": 25,
  "pageFileShareCapBytes": 8589934592,
  "maximumUptimeDays": 14,
  "minimumVramBytes": 4294967296,
  "minimumFreeDiskSpaceBytes": 2147483648,
  "minimumFeatureLevel": "11_0"
}
```

## 玩家用法

解压完整工具包，运行 `ZEnvironmentCollector.exe`，填事故三要素（问题类型、发生时间、可选的游戏目录），点"开始采集并生成 ZIP"；完成后点"打开目录"，确认内容后手动把 ZIP 发给客服。默认输出到桌面 `EnvironmentReports`。

一次跑完并打进同一个 ZIP：硬件与系统信息、事件诊断、双显卡选卡验证（`gpu-selection.json`）、DLL 静态检查与隔离加载探针（`compatibility.json`），配置了日志目录时附带最新的游戏日志副本。

**不做什么**：不读取序列号、不运行内存测试、不重启、不改 BIOS、不注入或启动游戏、不上传任何东西。权限不足、字段缺失、事件覆盖不完整会记在报告里——那不代表硬件健康，也不代表有问题。`compatibility` 阶段会在隔离子进程里加载 `GameCore.dll`（`ZDllLoadProbe.exe`，进程外执行），所以这一步不是只读。

### 高级：捕获一次崩溃

高级区用 Windows 系统转储捕获崩溃：选择当前项目游戏 exe → 点"开始" → 确认完整内存隐私与注册表修改 → 批准 UAC → 界面提示已启用后，正常启动游戏并复现崩溃 → 点"安全停止捕获"。默认等 1 小时。

- 一次启用可连续收集多次崩溃（单次会话上限 8 份：`crash.dmp`、`crash-2.dmp`…），停止、超时或达到上限后才统一恢复系统设置（可能再次弹 UAC）。
- 会临时改该 exe 名下的系统 WER 设置并把原值备份下来，结束后恢复；UAC 拒绝或设置已被其它工具改过时明确提示，窗口退出前对未恢复的配置再次告警。
- 结果在 `EnvironmentReports/crash-capture-wer-<GUID>`，分享包是里面的 `capture.zip`；没拿到 dump 时只有 `capture-failure.zip`，不冒称含 dump。
- 完整 dump 可能含账号、聊天、令牌和完整路径，无法脱敏，仅本地保存、ZIP 未加密、不自动上传——只走人工安全渠道发送。
- 启用成功**不保证**系统一定产生 dump（自定义崩溃处理、自动调试设置或系统策略都可能阻止它），也不用于卡死或正常退出。
- "选择已有 dump 并打包"不改注册表、不删除或移动原文件，验证后复制打包，报告里记为导入而不是本次运行产生。
