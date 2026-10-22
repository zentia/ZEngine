# Windows Environment Check

`ZEnvironmentCollector` 是独立的 Windows x64 环境检查与采集工具：在不启动游戏客户端的前提下收集硬件信息、检查 DLL 构建配置，并通过隔离子进程验证 `GameCore.dll` 能否加载。

## 一个程序，两种用法

只有一个 `ZEnvironmentCollector.exe`，图形界面和命令行都在它里面：

```powershell
.\ZEnvironmentCollector.exe                    # 图形界面，只有一页
.\ZEnvironmentCollector.exe -batchmode --help  # 命令行，无窗口
```

`-batchmode` 沿用 Unity/UE 的写法：不开窗口，结果写 stdout，退出码交给调用方。GUI 模式只认识 `--profile` / `--output-dir` / `--incident-time` / `--lang`，其它参数直接报错。

命令行在 cmd.exe 与 PowerShell 下 stdout/stderr 与 `%ERRORLEVEL%` / `$LASTEXITCODE` 照常，输出被重定向到管道或文件时不会被抢走；bat 里会等它结束，`exe && next` 可用。

它**刻意不是** ZEditor 的 ToolSuite：这个工具要发到玩家机器上，所以是 /MT 静态链接、机器无需 VC redist——合进编辑器不会让这份 exe 消失，只会多出一份要同步维护的实现。理由见 `engine/Source/Editor/ToolSuite/ToolSuite.h` 与 `engine/CMakeLists.txt` 里 `ZENGINE_BUILD_WINDOWS_ENVIRONMENT_CHECK` 的注释。

界面只有一页，对应 `-batchmode --collect`：一次跑完并把所有结果打进同一个 ZIP——硬件、事件诊断、双显卡选卡验证（`gpu-selection.json`）、DLL 静态检查与隔离加载探针（`compatibility.json`），以及崩溃捕获高级区。

因此 `--collect` **不是只读**：探针会在隔离子进程里 `LoadLibraryExW` 并调用 `OSG_LocalPredictHardwareCapable()`。仍未改变的是：不上传、不压测、不运行内存测试、不重启、不改 BIOS、不注入或启动游戏。

## 日志

工具自身的诊断落盘到 `%USERPROFILE%\AppData\LocalLow\ZEngine\Logs\ZEnvironmentCollector_YYYYMMDD_N.log`（与引擎日志同目录）。字体缺失、D3D 初始化失败、后台任务报错等只能在那里看到。`manifest.json` 的 `toolLog` 字段记录本次实际写入的文件，日志本身不进 ZIP。

**一次只允许一个实例**。第二个直接退出（退出码 `31`；命令行在 stderr 说明原因，GUI 弹提示框让用户先关掉原来的窗口）。豁免两种：`--help`；内部 worker 子进程（`--collect-stage` / `--wer-config`）——它们是父进程还活着时必须能启动的自己的副本。

**worker 子进程完全不记日志**：父进程正占着当天的日志文件，子进程必然抢不到；它们以 `CREATE_NO_WINDOW` 且不继承句柄的方式启动，连报错都无处可打。stage 的失败通过退出码交给父进程（manifest 里体现为 `INCOMPLETE`），往 stage 里加 `LogError()` 写不到任何地方。

## 检查内容

### 系统与硬件

- Windows 版本及 x64 架构
- CPU 型号、Family/Model/Stepping、逻辑处理器数量
- SSE4.1、SSE4.2、AVX、AVX2、FMA、F16C、AVX-512；AVX/AVX-512 的 OSXSAVE 与 XCR0 状态
- 物理内存、可用内存、页面文件、系统运行时间
- GPU 型号、PCI ID、显存、驱动版本和驱动日期
- 显示器、分辨率、刷新率和色深
- 物理磁盘型号、总线类型和容量；卷、文件系统、总容量及剩余空间
- 双显卡自动选卡验证：10 个离线回归用例（与 `GfxDeviceSetup.cpp` 的回退启发式逐字节一致）+ 本机 `EnumAdapterByGpuPreference` 真实探测

用例**断言两件事**：选中哪张卡，以及**靠什么理由选中**（排除集显，还是退回"显存最大"）——只断言下标时，"找到非集显卡"写成 `> 0` 这种差一错误有 4 个用例下标依旧正确，而它在真卡排第一位的机器上就会选中集显。5 个真实场景（集显虚报显存、单卡、纯集显、真卡排在前而集显报得更多…），5 个钉住分支语义：软件适配器必须被排除、"严格大于"、软件判定是合取（`vendor==0x1414 && deviceId==0x8c`）、Intel `0x8086` 一律算集显（Arc 独显会输给显存更小的他厂卡，刻意钉住现行为）、只有软件适配器时仍是答案而不是失败。

选卡验证由 `CollectEnvironment()` 采集，是环境快照的一部分，因此 `-batchmode` 报告与 `--collect` 写的 `gpu-selection.json` 是同一份数据（共用 `BuildGpuSelectionJson()`）。

### DLL 静态检查

`--collect` 的 `compatibility` 阶段做这件事（结果进 `compatibility.json` 并打包）。扫描根是**工具自身目录 + `--game-dir` 指定的目录**；命令行 `-batchmode` 默认命令的扫描根是工具自身目录 + 当前工作目录。递归扫描其中所有 `.dll`：PE 有效性、x64 架构、导入库；Debug CRT（`MSVCP*D.dll`、`VCRUNTIME*D.dll`、`ucrtbased.dll` 等）；用 Zydis 扫描 x64 Runtime Function 范围中的 AVX、AVX2、FMA、F16C、AVX-512 指令，输出指令 RVA、文件偏移、机器码及反汇编样例。

静态包含 AVX/AVX2 不代表旧 CPU 一定执行该指令（受 CPUID 保护的优化路径允许存在），静态扫描只提供风险信息，不直接判定加载失败。

### DLL 动态探针

`GameCore.dll` / `GameCore_Standalone.dll` 自动进入探针，也可用 `--probe-dll` 显式指定。探针在独立进程中 `LoadLibraryExW`，对 GameCore 默认查找并调用 `OSG_LocalPredictHardwareCapable()`，捕获非法指令、访问违规、栈溢出和 DLL 初始化异常，把 Win32 错误、异常码、导出函数返回值和子进程输出写入报告。

`OSG_LocalPredictHardwareCapable()` 返回 `0` 只表示这个可选能力不可用（旧 CPU 上常见），**不算加载失败**——只要 DLL 加载成功且没有非法指令，报告里对应一条 warning。

## 基本使用

将工具与待测 DLL 放在同一目录：

```powershell
.\ZEnvironmentCollector.exe -batchmode --output-dir .\EnvironmentReport
.\ZEnvironmentCollector.exe -batchmode --require avx,avx2,fma,f16c --min-memory-gb 8 --output-dir .\EnvironmentReport
.\ZEnvironmentCollector.exe -batchmode --check-dll .\GameCore.dll --output-dir .\EnvironmentReport
.\ZEnvironmentCollector.exe -batchmode --require none --probe-dll .\GameCore.dll --output-dir .\EnvironmentReport
```

默认支持无 AVX 的旧 CPU，产品有明确要求时用 `--require` / `--min-memory-gb` 收紧。只想看机器、不想等递归 DLL 扫描和隔离加载探针时加 `--no-dll-checks`（只作用于 `-batchmode` 默认命令）。

## 报告

只生成 `OSGameEnvironmentReport.json`。同一份内容的文本版不再落盘，只打印到 stdout（`--quiet` 可关），要留档自己重定向：`-batchmode > report.txt`。

```json
{
  "schemaVersion": 5,
  "toolVersion": "1.8.1"
}
```

## 退出码

| 退出码 | 含义 |
|---:|---|
| `0` | 检查通过，可能包含非阻断警告 |
| `10` | CPU 不满足显式配置的最低 ISA |
| `11` | 不是受支持的 Windows x64 环境 |
| `12` | 物理内存不足 |
| `20` | DLL 依赖不可分发的 Debug CRT |
| `21` | DLL/PE 无效、架构不支持或静态检查失败 |
| `23` | DLL 加载、隔离探针、导出调用或非法指令失败 |
| `24` | 探针程序或基础设施不可用 |
| `30` | 主工具内部错误 |
| `31` | 已有实例在运行，本次未执行 |

常见 `issueCode`：`DLL_DEBUG_RUNTIME_UNSUPPORTED`、`DLL_LOAD_FAILED`、`DLL_PROBE_ILLEGAL_INSTRUCTION`、`DLL_PROBE_EXCEPTION`、`DLL_PROBE_TIMEOUT`、`DLL_PROBE_EXPORT_NOT_FOUND`、`DLL_PROBE_INFRASTRUCTURE_FAILED`。

## 事件诊断与玩家一键采集

### 事件扫描顺序

按时间正序扫描整个回溯区间，命中 `MaxRelevantEvents`（5000）上限后丢弃的会是**最近的**事件，所以扫描锚定到事故时刻而不是把区间整体向前扩大：

- 提供 `--incident-time` 时，`BuildEventScanOrder()` 把区间切成 3 段并按序扫描：① `[事故-30min, 事故+30min]`（决定 verdict 的窗口）→ ② 事故之后（Windows 延迟写入的 bugcheck / WER）→ ③ 更早的上下文。上限或超时只会牺牲上下文；三段边界互不重叠（闭区间之间留 1 tick）。
- 每段用 `EvtQueryReverseDirection` 倒序读取；两个通道（System / Application）都先扫优先窗口再扫上下文，因为同一条规则（如 `GPU_TDR`）的证据可能来自任一通道。
- 读取后把 `events` 与 `systemEventTimes` 重新按时间升序排序（规则与顺序无关，但事件文件要给人看、也要能回放）。
- 证据不在事故时刻附近时，`SYSTEM_ISSUES_NEARBY` 的文案明确说"该记录不在问题发生的时间点附近，无法确定与本次问题有关"。

截断分两种，靠 `Observability` 的两个字段区分：

| 字段 | 含义 |
|---|---|
| `priorityWindowRequested` | 本次是否请求了优先窗口（等价于"是否提供了事故时间"） |
| `priorityWindowComplete` | 优先窗口是否被完整扫完。它排在最前，因此扫完它之后才出现的截断只影响上下文 |

两者都为真时截断只记 `SCAN_CONTEXT_TRUNCATED`，不再置 `scanIncomplete`，无发现的结论保持 `NO_SYSTEM_EVIDENCE`（`confidence=medium`）；截断发生在优先窗口内则仍判 `INCONCLUSIVE`；优先窗口落在采集区间之外时按单段扫描，`priority` 为 false，结论保守。

契约：事件明细文件版本 `1` → `2`（新增两个布尔键），解析器接受 `1` 与 `2`、旧样本仍可回放、缺失的键取默认值 false、版本超范围时报明确错误。`OSGameDiagnosis.json` 的 `schemaVersion` 保持 `1`（只在 `observability` 下新增两个字段），launcher 只读 `verdict` / `confidence` / `userMessage` / `actions`，不受影响。

### 事故锚点与 isTargetGame

采集包要能被归因，必须同时有 `incidentTimeUtc` / `incidentKind` / `gameExecutable`；缺任一项就既不能证明"是游戏导致的"，也不能证明"与游戏无关"。缺项时：

| 出口 | 行为 |
|---|---|
| `summary.txt` | **顶部**输出分隔线包裹的中文警告，逐项列出缺什么 |
| `manifest.json` | 新增 `"attributionUsable"` 与 `"missingAttributionInputs":[...]`，供分诊脚本在人工打开之前过滤 |
| GUI | 在项目名下方常驻红色提示（改类型/填时间即时刷新），点「开始采集」时再弹一次确认 |
| CLI | `-batchmode --collect` 在 stdout 打印同一段警告 |

不阻断采集：纯机器体检本来就是合法用法，只是不能让它冒充具备归因价值。

`events-redacted.json` 的 `isTargetGame`（schemaVersion `2`）只回答"崩溃的是不是指定的游戏"这个布尔问题，不泄露进程名：

| `gameExecutableSpecified` | `isTargetGame` | 含义 |
|---|---|---|
| false | `null` | 没给游戏 exe，无法判定（**不是** false） |
| true | `true` / `false` | 等于 / 不等于指定的游戏 exe |

`module` 只在**该文件已经点名了这个进程**时才可读（指定的游戏、或 `dwm.exe`）：崩溃发生在 exe 自身时 `ModuleName` 往往就是那个进程名，`application` 解析不出来时若保留 `module`，等于把 `OTHER_PROCESS` 想藏的名字原样交出去。其余一律 `REDACTED`。

### 报告文件的发布方式（launcher 异步读取契约）

launcher 不等工具跑完再读结果，而是工具还在跑时就轮询输出目录。所以所有报告文件（`OSGameDiagnosis.json`，以及 `--collect` 各阶段的 `*.json` / `summary.txt`）都**先写临时文件、再 `MoveFileEx(REPLACE_EXISTING)` 改名发布**（`ReportWriter.cpp` 的 `WriteReportFile`）：

- 临时文件是同目录下的 `<目标名>.<pid>.tmp`，无论发布成功还是失败都不会留下它；读者只会看到"旧的完整文件"或"新的完整文件"。
- 发布需要目标文件的 DELETE 权限，而 MSVC 的 `std::ifstream` 只共享读和写、**不共享删除**：读者一直开着文件不放会让发布失败。工具重试约 1 秒再放弃，放弃时抛异常且不留下半成品，等价于"本次没有产出报告"——launcher 应按"没检查过"处理（fail-open）。**读报告请用 `FILE_SHARE_DELETE` 打开，读完立刻关闭。**
- 覆盖已有报告的行为不变（每次运行都重写）；与 ZIP 相反，`ReportArchive.cpp` 的 `MoveFileW` 故意不允许覆盖已发布的证据包。

### --preflight：每次拉起时的机器体检

```
ZEnvironmentCollector.exe -batchmode --preflight [--output-dir <path>] [--game-dir <path>]
                          [--min-spec <path>] [--game-exe <name>] [--budget-seconds N] [--quiet]
```

产出 `OSGamePreflight.json`，与诊断报告用同样的四个键 `verdict` / `confidence` / `userMessage` / `actions`；launcher 用同一套解析就能读两个文件，`"mode":"preflight"` 用来区分。

| verdict | 含义 | launcher 应当 |
|---|---|---|
| `Pass` | 没发现值得说的 | 不提示 |
| `Advise` | 可提示的环境风险（见下） | 非模态提示，同一个提示只说一次 |
| `BelowMinimum` | 缺 AVX2、内存低于最低要求（默认 4 GB）、非 64 位 Windows、没有任何一张显卡能初始化 Direct3D 11、只有软件渲染器能初始化 D3D11（驱动没装）、功能级别低于 `minimumFeatureLevel` | 明确提示，但不阻断启动 |
| `Inconclusive` | 没跑完（超时或探针失败） | 当作"没检查过" |

`Advise` 覆盖：同厂商两张卡驱动版本不一致、选到集显、显示器接在集显上（仅台式机）、只有软件渲染、可用内存过低、页面文件过小或未启用、未检测到 VC++ 运行库、游戏默认要用的显卡无法初始化 D3D11（另一张可以）、显存低于游戏要求、游戏盘或系统盘空间不足、有更新或驱动装完在等重启、很久没重启、最近显卡驱动崩溃重置过、游戏装在机械硬盘上、装在 U 盘/SD 卡/网络盘或非 NTFS 分区、安装路径含非 ASCII 字符（系统代码页非 UTF-8 时）、预览版系统。

- **限时**：默认最多 3 秒（`--budget-seconds` 1–30），到点按 `Inconclusive` 结束。已有实例在跑时直接退出且不写文件。
- **只读**：CPU、已装与可用内存、页面文件、运行时间、系统版本、显卡选卡、每张卡接了几个显示器、机箱类型、VC++ 运行库、游戏卷与系统卷可用空间（后者仅在与游戏卷不同盘时判）。不扫盘、不加载游戏 DLL、不写注册表、不提权；唯一例外是下面那条事件日志查询。
- **内存看两个数**：已装内存低于最低要求 → `BelowMinimum`；已装够但**当前可用**很少 → `Advise`。
- **显卡只问 D3D11 能不能初始化**（Windows 客户端只跑 D3D11：`ProjectSettings.asset` 的 `m_APIs: 02000000`、`m_Automatic: 0`）。必须建设备才能问（代价热 85–95 ms、冷最高 0.9 s，在预算内跑）；钉在游戏会选的那张卡（`EnumAdapterByGpuPreference(HIGH_PERFORMANCE)`）；软件适配器只记录不算数，`D3D11_ONLY_SOFTWARE_ADAPTER`（能起但帧率极差）与 `D3D11_DEVICE_UNAVAILABLE`（真跑不起来）是两个结论；首选的卡不行时去问其它卡，一张都不行才 `BelowMinimum`；级别阶梯 `11_1 / 11_0 / 10_1 / 10_0` 与引擎一致。`d3d11.dll` 运行时加载，不硬链接。
- **页面文件看两个数**：绝对下限，以及占物理内存的比例（上限 `pageFileShareCapBytes`，默认 8 GB）；内存 ≥ `pageFileRuleMaxMemoryBytes`（默认 16 GB）且根本没设页面文件时不提示。
- **等重启**：CBS `RebootPending`、Windows Update `RebootRequired`、`PendingFileRenameOperations` 里**指向驱动目录的那几条**（`\DriverStore\`、`\System32\drivers\`）。
- **最近显卡驱动崩溃重置**：preflight 里唯一读事件日志的规则，只看近 7 天的 TDR（Display 4101、LiveKernelEvent 117/141/142），判据与完整采集共用同一个函数；倒序读取 + 独立 600 ms 截止时间，两个通道都读不到就是 `known=false`。
- **其它**：开机时长 14 天（`maximumUptimeDays`，写 0 关掉）；游戏卷类型（`GetDriveTypeW`）与文件系统（非 NTFS 提示）；安装路径含非 ASCII（系统 ANSI 代码页 `65001` 时不提示，ACP 从注册表 `HKLM\...\Nls\CodePage\ACP` 读）；显存阈值默认关（`minimumVramBytes` 默认 `0`）；机械硬盘走 `IOCTL_STORAGE_QUERY_PROPERTY`；系统盘是另一个盘时空间单独算一份。
- 报告里的路径走 `SanitizePath`，用户名不进报告。**输出目录默认 `%LOCALAPPDATA%\OSGame\EnvCheck`**（不是当前目录：launcher 拉起时当前目录是游戏安装目录）。**退出码只有 0 和 30**：0 = 结论已写入，30 = 工具自身出错。

### 最低配置不由工具决定：min-spec.json

最低配置是**游戏**的属性，不编译进 exe，而是随游戏发布、每次拉起时读取（默认 `<游戏目录>\min-spec.json`，可用 `--min-spec <path>` 覆盖）：

```json
{
  "avx": false,
  "avx2": true,
  "fma": false,
  "f16c": false,
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

- **只认这一种形状**：扁平对象，值是布尔、整数，或 `minimumFeatureLevel` 这一项的名称字符串；嵌套、数组、其它键上的字符串一律拒绝。
- **没写的键沿用内置默认**；**`0` 是关掉这条规则**，不是"要求 0"。
- **解析失败一律回落**：文件不存在、打不开、写坏了都用内置默认继续跑，并把原因写进报告的 `notes`。
- **`minimumFeatureLevel` 是唯一用文本写的阈值**（`"11_0"`、`"10_1"`…），写成裸数字会被拒；不写＝不限。
- **认不出的键会被点名**：跳过并记进 `notes`，其余键照常生效。
- 报告里有 `"requirements"`（本次判定用的全部阈值）和 `"requirementsSource"`（`default` 或 `file:<路径>`）。

### 玩家操作

解压完整工具包，运行 `ZEnvironmentCollector.exe`，填事故三要素（问题类型、可选填带时区的发生时间）、可选填**游戏目录**（`--game-dir` 的同义输入框），点"开始采集并生成 ZIP"，一次拿到全部结果：CPU、内存、BIOS/整机/主板型号、SMBIOS 内存配置、原始微码注册表字节、显卡/驱动、双显卡选卡验证（`gpu-selection.json`）、磁盘容量、事件诊断、WHEA 及已有内存诊断结果、**DLL 静态检查与隔离加载探针（`compatibility.json`）**，并在项目配置了日志目录时附带最新的游戏日志副本。默认输出到桌面 `EnvironmentReports`。

- 不读取序列号、不运行内存测试、不重启、不改 BIOS。每个采集阶段在独立受 Job 管理的子进程执行，默认 45 秒超时；失败/取消保留已完成信息。权限不足、字段缺失、事件覆盖不完整会记录在报告中，不代表硬件健康。
- GUI 默认勾选"同意附带事件明细"，可手动取消；CLI 仍需显式 `--include-events`。窗口右侧有可拖动滚动条，也支持滚轮或点击轨道翻页，尺寸随 DPI 缩放。完成后点"打开目录"，确认内容后手动发送 ZIP；不自动上传。
- `compatibility` 阶段会在隔离子进程里加载 `GameCore.dll` / `GameCore_Standalone.dll`（`ZDllLoadProbe.exe`，无 CRT、无 AVX，进程外执行），所以这一步**不再只读**。`--game-dir` 不给时扫描根只有工具自身目录。
- 上限：最多 64 个 DLL（`MaxDllCheckFiles`），成文后超过 8MiB 从**列表尾部**丢弃重建，两种情况都记 `INCOMPLETE` 并在 `manifest` 里体现 `partial`。`GameCore` 永远排在最前。该阶段超时下限 180 秒（`--stage-timeout-seconds` 可调）。归档单文件上限 16MiB，超限会导致整个包打包失败。

微码仅保存 `Update Revision` 原始字节，不声称跨 CPU 统一解码；内存标称/配置速率不等于 XMP 判定；BIOS 日期不是安装日期；没有 WHEA 或内存检测结果不等于通过检测。

### 游戏日志打包（gamelogs 阶段）

把游戏日志目录里**窗口内最新的** `.log` / `.logcompr` / `.txt` 复制进报告目录并打进 ZIP，索引写入 `game-logs.json`。`.logcompr` 是游戏在 `Cache\Logs` 下写的压缩会话日志，漏掉它就等于没有游戏自己的记录。

- 目录由 profile 声明，不猜测：`logRoot`（`localLow` 默认，另支持 `localAppData` / `roaming` / `programData`）+ `logSubPath`（相对路径，禁止绝对路径、`..`、驱动器号）。OSGame 配置为 `logRoot=localLow` + `logSubPath=Tencent\osgameCNOfficial\Cache\Logs`。未配置 `logSubPath` 的 profile 不运行该阶段。CLI 可用 `--log-dir <绝对路径>` 覆盖。
- 挑选：先取事故时间 ±窗口内写过的日志，其余按最后写入时间倒序；上限 8 个文件、单文件 16MiB、合计 48MiB，超限文件不截断、只记 `skipped`（`TOO_LARGE` / `TOTAL_LIMIT` / `UNREADABLE` / `FILE_LIMIT`）。
- 只复制不修改：以 `FILE_SHARE_READ|WRITE|DELETE` 打开，游戏仍在写也能读；**不跟随目录/文件链接**（重解析点在递归之前就排除）。副本命名 `gamelog-NN-<原名>.log`，原文件不改名、不删除、不截断。只扫描日志目录及其下一层子目录。
- 索引记录原始相对路径、字节数、修改时间、SHA256 与 `complete`（复制期间被追加时为 false）：写完后再问一次原文件大小，比复制到的更长就说明**尾巴没进来**。日志目录路径用 `SanitizePath` 去掉用户名。
- 目录不存在或窗口内没有日志时返回 `NO_LOGS`，**不计入 `partial`**；`game-logs.json` 仍写入并给出 `reason`（`NOT_CONFIGURED` / `LOG_DIRECTORY_MISSING` / `NO_LOG_FILES_IN_WINDOW` / `NO_LOG_FILES_COLLECTED`）。
- 隐私：日志可能含账号、聊天等信息。副本只留在本地报告目录，不自动上传；`summary.txt` 在包含日志副本时追加提示，发送前需人工确认。

### CLI 与配置

```powershell
.\ZEnvironmentCollector.exe -batchmode --collect --profile .\profiles\osgame.ini --output-dir .\Reports --include-events
.\ZEnvironmentCollector.exe -batchmode --diagnose --diagnose-detail --incident-time 2026-10-12T18:30:00+08:00 --incident-kind crash --output-dir .\Diagnosis
.\ZEnvironmentCollector.exe -batchmode --diagnose --diagnose-from-events .\Diagnosis\OSGameDiagnosisEvents.tsv --output-dir .\Replay
.\ZEnvironmentCollector.exe -batchmode --gpu-selection
.\ZEnvironmentCollector.exe --profile .\profiles\osgame.ini          # GUI，指定 profile
.\ZEnvironmentCollector.exe --lang zh-CN                             # 界面语言（默认英文）
```

`--collect` 还支持 `--incident-time`、`--incident-kind crash|hang|unknown`、`--lookback-hours 1..720`、`--stage-timeout-seconds 1..300`、`--log-dir <游戏日志目录>`；未填事故时间按已有事件查找，不伪装成当前时间。退出 0 表示 ZIP 生成，完整性见 manifest；30 为错误。`--gpu-selection` 打印离线回归用例与本机真实探测：回归全通过退 0，任一用例选错退 1（说明 `GfxDeviceSetup.cpp` 的选卡逻辑回退了），工具异常退 30；本机不支持 `EnumAdapterByGpuPreference` 不算失败。

`--collect-stage` 与 `--wer-config` 是内部入口：采集器以受 Job 管理的子进程、或提权 `ShellExecute` 的方式重新启动自己（`ToolExecutablePath()`）。它们即使不带 `-batchmode` 也不会弹窗，玩家不要直接调用。

GUI 默认读取同目录 `profiles/default.ini`；其他项目只需设置 `[project]` 下的 `id`、`displayName`、`gameExecutable`、`lookbackHours`，需要附带游戏日志时再加 `logRoot`（默认 `localLow`）与 `logSubPath`。配置支持 ASCII 或 UTF-16LE BOM 编码，项目 ID 与进程名仅允许安全文件名字符；无目标游戏时使用 generic，不能把其它进程推断成目标游戏。

普通 CLI 并不是安全采集入口：`--check-dll` 会追加路径而不是关闭递归扫描。玩家用 GUI 或 `--collect`。

### Windows 系统转储（WER）与已有 dump 导入

高级区只有 Windows 系统转储一条路：选择当前项目游戏 exe → 点击开始 → 确认完整内存隐私及注册表修改 → 批准 UAC → 界面提示系统捕获已启用后，正常启动游戏并复现崩溃。默认等 1 小时。目标需在当前用户会话运行；不同用户的游戏不会自动放宽输出目录权限。

- **一次启用可连续收集多次崩溃**：每次崩溃各复制一份（`crash.dmp`、`crash-2.dmp`…，单次会话上限 8 份），点"安全停止捕获"、等待超时或达到上限后才统一恢复注册表。每份 dump 独立做完整内存/异常线程/PID/模块路径校验，晚到的 dump 不会被当重复文件跳过。
- **只写 64 位注册表视图** `HKLM/SOFTWARE/Microsoft/Windows/Windows Error Reporting/LocalDumps/<exe文件名>` 三值：DumpType DWORD=2、DumpCount DWORD=2、DumpFolder REG_EXPAND_SZ=本次私有目录。WER 按文件名匹配而非完整路径，所以采集器要结合观测到的 PID、创建时间、dump 异常时间和完整模块路径才能自动接收；身份证据不足时保留原文件并提示手工确认，不把任意新文件当目标崩溃。仅做有限查询，不申请 VM_READ、不调试附加、不修改反作弊。
- **提权只在启用/恢复时发生**，走随包 CLI 的 `--wer-config` 内部入口，参数不进入 shell。备份写在 `HKLM/SOFTWARE/ZEngine/EnvironmentCollector/WerBackupsV1/<exe>`，ACL 只允许管理员/SYSTEM 修改；恢复逐值比较，若其他工具已改动则返回冲突并保留备份。**恢复是恢复先前配置，不是关闭已有全局 WER。** UAC 拒绝/恢复冲突时明确提示，窗口退出前对未恢复配置再次告警。
- **结束收集后**按原配置恢复（可能再次弹 UAC），再补采环境、算 SHA256、ZIP64 压缩。多份 dump 打进同一个 `capture.zip`，`checksums.json` 逐份记录 SHA256；`capture.json` 有 `capturedDumpCount` 与 `dumps` 数组（PID、异常码、dump 时间），顶层单份字段仍指向第一份以兼容旧读取器。用户主动停止且已收到 dump 时按正常完成处理；一份都没收到时仍为取消/超时失败。
- **会话目录** `EnvironmentReports/crash-capture-wer-<GUID>`：`system-dumps` 保留 Windows 原文件，验证后的副本为 `crash.dmp`，最终分享包是该私有目录内的 `capture.zip`。目录权限限当前用户、管理员和 SYSTEM；不跟随目录链接，不覆盖已有文件。空间不足/超时/后处理失败生成仅含 `capture.json` 的 `capture-failure.zip`，不冒称含 dump。复制前要求额外约 2 倍 dump 大小 + 1GiB（至少 4GiB 起步），单 dump 上限 64GiB。
- **"选择已有 dump 并打包"**不改注册表，不删除或移动原文件；验证后复制、补采与打包，报告记 `captureBackend=dump_import` 与 `identity=USER_CONFIRMED_IMPORT_AND_MODULE_PATH`，不伪称属于本次运行。WER/import 报告 schemaVersion=2。
- **限制**：真实 WXQ.exe 及反作弊环境未验证；自定义崩溃处理、自动调试设置或系统策略可能阻止 WER，配置成功不保证生成 dump，不用于卡死/普通退出。**跨用户目标、标准用户 UAC 拒绝、异机 ACL/受控文件夹三项仍未验收**。只支持安全 exe 文件名、本地普通目录；工具未数字签名、不自动上传。参考：<https://learn.microsoft.com/en-us/windows/win32/wer/collecting-user-mode-dumps>。
- **验收**：本机已提权会话（2026-10）走通启用 → 连续捕获两份 → 恢复，`LocalDumps` 与备份键均无残留。手工命令 `EnvironmentWerTests.exe --consent-system-test normal <目录>`（未挂进 ctest），**必须与 `ZEnvironmentCollector.exe` 同目录**：提权重启的是同目录的自己，缺了它不会弹 UAC，只报"同目录缺少 `ZEnvironmentCollector.exe`"。

捕获结果与普通报告分开保存："打开捕获结果"只使用本窗口最近一次捕获结果，不受普通采集影响；重开窗口无历史结果时提示到桌面 `EnvironmentReports/crash-capture-*` 查找，不拿普通报告冒充。

- **打包**：成功后所有证据文件算 SHA256，按白名单流式 Deflate 压成 ZIP64 并实际解压回读 CRC（64KiB 读缓冲，不把数 GB 文件装入内存）；大 dump 打包可取消。压缩依赖 `engine/3rdparty/minizip-ng`（deflate 单方法，ZIP64 原生支持），交付源码须包含 `LICENSE-minizip-ng.txt`。
- **隐私**：完整 dump 可能含账号/聊天/令牌和完整路径，无法脱敏，仅本地保存、经安全渠道手动发送；ZIP 本身未加密，不自动上传。
