# Windows Environment Check

`ZEnvironmentCollector` 是独立的 Windows x64 环境检查与采集工具：在不启动游戏客户端的前提下收集硬件信息、检查 DLL 构建配置，并通过隔离子进程验证 `GameCore.dll` 能否加载。

## 一个程序，两种用法

只有一个 `ZEnvironmentCollector.exe`，图形界面和命令行都在它里面：

```powershell
.\ZEnvironmentCollector.exe                    # 图形界面，只有一页
.\ZEnvironmentCollector.exe -batchmode --help  # 命令行，无窗口
```

`-batchmode` 沿用 Unity/UE 的写法：不开窗口，结果写 stdout，退出码交给调用方。GUI 模式只认识 `--profile` / `--output-dir` / `--incident-time` / `--lang`，其它参数直接报错。

命令行在 cmd.exe 与 PowerShell 下 stdout/stderr 照常、`%ERRORLEVEL%` 与 `$LASTEXITCODE` 照常；输出被重定向到管道或文件时不会被抢走。bat 里会等它结束，`exe && next` 可用。

它**刻意不是** ZEditor 的 ToolSuite：这个工具要发到玩家机器上，所以是 /MT 静态链接、机器无需 VC redist——合进编辑器不会让这份 exe 消失，只会多出一份要同步维护的实现。完整理由见 `engine/Source/Editor/ToolSuite/ToolSuite.h` 的"哪些东西不该做成套件"与 `engine/CMakeLists.txt` 里 `ZENGINE_BUILD_WINDOWS_ENVIRONMENT_CHECK` 的注释。

界面只有一页，对应 `-batchmode --collect`：一次跑完并把所有结果打进同一个 ZIP——硬件、事件诊断、双显卡选卡验证（`gpu-selection.json`）、DLL 静态检查与隔离加载探针（`compatibility.json`），以及崩溃捕获高级区。

因此 `--collect` **不是只读**：探针会在隔离子进程里 `LoadLibraryExW` 并调用 `OSG_LocalPredictHardwareCapable()`。仍未改变的是：不上传、不压测、不运行内存测试、不重启、不改 BIOS、不注入或启动游戏。

## 日志

工具自身的诊断落盘到

```text
%USERPROFILE%\AppData\LocalLow\ZEngine\Logs\ZEnvironmentCollector_YYYYMMDD_N.log
```

与引擎日志同目录。字体缺失、D3D 初始化失败、后台任务报错等只能在那里看到。`manifest.json` 里的 `toolLog` 字段记录本次实际写入的那个文件，日志本身不进 ZIP。

**一次只允许一个实例**。第二个直接退出（退出码 `31`；命令行在 stderr 说明原因，GUI 弹提示框让用户先关掉原来的窗口），而不是"报告照常生成、日志悄悄丢失"。豁免两种：`--help`；内部 worker 子进程（`--collect-stage` / `--wer-config`）——它们是父进程还活着时必须能启动的自己的副本。

**worker 子进程完全不记日志**：父进程正占着当天的日志文件，子进程必然抢不到；而它们以 `CREATE_NO_WINDOW` 且不继承句柄的方式启动，连报错都无处可打，所以干脆不初始化。stage 的失败通过退出码交给父进程（manifest 里体现为 `INCOMPLETE`）。往 stage 里加 `LogError()` 写不到任何地方。

## 检查内容

### 系统与硬件

- Windows 版本及 x64 架构
- CPU 型号、Family/Model/Stepping、逻辑处理器数量
- SSE4.1、SSE4.2、AVX、AVX2、FMA、F16C、AVX-512
- AVX/AVX-512 的 OSXSAVE 与 XCR0 状态
- 物理内存、可用内存、页面文件、系统运行时间
- GPU 型号、PCI ID、显存、驱动版本和驱动日期
- 显示器、分辨率、刷新率和色深
- 物理磁盘型号、总线类型和容量
- 卷、文件系统、总容量及剩余空间
- 双显卡自动选卡验证：10 个离线回归用例（与 `GfxDeviceSetup.cpp` 的回退启发式逐字节一致）+ 本机 `EnumAdapterByGpuPreference` 真实探测

选卡用例**断言两件事**：选中哪张卡，以及**靠什么理由选中**（排除集显，还是退回"显存最大"）。下标对了而理由错了也算失败——只断言下标时，把"找到非集显卡"写成 `> 0` 这种差一错误有 4 个用例下标依旧正确，而它在一台真卡排第一位的机器上就会选中集显。5 个是真实场景（集显虚报显存、极端虚报、单卡、纯集显、真卡排在前而集显报得更多），5 个钉住分支语义：**软件适配器必须被排除**、**"严格大于"**（两块同型号卡时第一块保持当选）、**软件判定是合取**（`vendor==0x1414 && deviceId==0x8c`，只认厂商会把真卡踢掉）、**Intel `0x8086` 一律算集显**（所以 Arc 独显会输给显存更小的他厂卡——刻意钉住现行为，将来真改这条要让用例红一次）、**只有软件适配器时仍是答案而不是失败**。

选卡验证由 `CollectEnvironment()` 采集，是环境快照的一部分，因此 `-batchmode` 报告与 `--collect` 写的 `gpu-selection.json` 是同一份数据（共用 `BuildGpuSelectionJson()`），不会出现两套结论。

### DLL 静态检查

`--collect` 的 `compatibility` 阶段会做这件事（结果进 `compatibility.json` 并打包）。扫描根是**工具自身目录 + `--game-dir` 指定的目录**；命令行 `-batchmode` 默认命令的扫描根是工具自身目录 + 当前工作目录。递归扫描其中所有 `.dll`：

- PE 文件有效性、x64 架构、导入库
- Debug CRT：`MSVCP*D.dll`、`VCRUNTIME*D.dll`、`ucrtbased.dll` 等
- 用 Zydis 扫描 x64 Runtime Function 范围中的 AVX、AVX2、FMA、F16C、AVX-512 指令，输出指令 RVA、文件偏移、机器码及反汇编文本样例

静态包含 AVX/AVX2 不代表旧 CPU 一定执行该指令（受 CPUID 保护的优化路径允许存在），静态扫描只提供风险信息，不直接判定加载失败。

### DLL 动态探针

`GameCore.dll` / `GameCore_Standalone.dll` 自动进入探针，也可用 `--probe-dll` 显式指定：

1. 在独立进程中 `LoadLibraryExW`；
2. 对 GameCore 默认查找并调用 `OSG_LocalPredictHardwareCapable()`；
3. 捕获非法指令、访问违规、栈溢出和 DLL 初始化异常；
4. 将 Win32 错误、异常码、导出函数返回值和子进程输出（探针自身不打印，但被加载的 DLL 可能从 `DllMain` 里写）写入报告。

`OSG_LocalPredictHardwareCapable()` 返回 `0` 只表示这个可选能力不可用（旧 CPU 上常见），**不算加载失败**——只要 DLL 加载成功且没有非法指令；报告里对应一条 warning。

## 基本使用

将工具与待测 DLL 放在同一目录：

```powershell
.\ZEnvironmentCollector.exe -batchmode --output-dir .\EnvironmentReport
```

默认支持无 AVX 的旧 CPU。产品明确要求某些指令集时可配置：

```powershell
.\ZEnvironmentCollector.exe -batchmode `
  --require avx,avx2,fma,f16c `
  --min-memory-gb 8 `
  --output-dir .\EnvironmentReport
```

只检查指定 DLL：

```powershell
.\ZEnvironmentCollector.exe -batchmode --check-dll .\GameCore.dll --output-dir .\EnvironmentReport
```

显式执行隔离加载：

```powershell
.\ZEnvironmentCollector.exe -batchmode --require none --probe-dll .\GameCore.dll --output-dir .\EnvironmentReport
```

只想看机器、不想等递归 DLL 扫描和隔离加载探针时加 `--no-dll-checks`（只作用于 `-batchmode` 默认命令）。

## 报告

只生成：

```text
OSGameEnvironmentReport.json
```

同一份内容的文本版不再落盘，只打印到 stdout（`--quiet` 可关）。要留档自己重定向：`-batchmode > report.txt`。

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

常见 `issueCode`：

- `DLL_DEBUG_RUNTIME_UNSUPPORTED`
- `DLL_LOAD_FAILED`
- `DLL_PROBE_ILLEGAL_INSTRUCTION`
- `DLL_PROBE_EXCEPTION`
- `DLL_PROBE_TIMEOUT`
- `DLL_PROBE_EXPORT_NOT_FOUND`
- `DLL_PROBE_INFRASTRUCTURE_FAILED`

## 事件诊断与玩家一键采集

### 事件扫描顺序

按时间正序扫描整个回溯区间，命中 `MaxRelevantEvents`（5000）上限后丢弃的会是**最近的**事件，所以扫描锚定到事故时刻，而不是把区间整体向前扩大：

- 提供 `--incident-time` 时，`BuildEventScanOrder()` 把区间切成 3 段并按序扫描：① `[事故-30min, 事故+30min]`（决定 verdict 的窗口）→ ② 事故之后（Windows 延迟写入的 bugcheck / WER）→ ③ 更早的上下文。上限或超时只会牺牲上下文。三段边界互不重叠（闭区间之间留 1 tick）。
- 每段用 `EvtQueryReverseDirection` 倒序读取，单段内命中上限时丢弃的是最旧的记录。
- 两个通道（System / Application）都先扫优先窗口再扫上下文，因为同一条规则（如 `GPU_TDR`）的证据可能来自任一通道。
- 读取后把 `events` 与 `systemEventTimes` 重新按时间升序排序：规则与顺序无关，但事件文件要给人看、也要能回放。
- `SYSTEM_ISSUES_NEARBY` 的文案区分远近：证据不在事故时刻附近时不再说"可能与你遇到的问题有关"，而是明确"该记录不在问题发生的时间点附近，无法确定与本次问题有关"。
- `systemLogReadable` 只由区间查询的结果决定；读取最旧事件的探针失败或日志为空只影响 `systemOldestKnown`，不会被误判成"日志不可读"而直接 fatal。

截断分两种，靠 `Observability` 的两个字段区分：

| 字段 | 含义 |
|---|---|
| `priorityWindowRequested` | 本次是否请求了优先窗口（等价于"是否提供了事故时间"） |
| `priorityWindowComplete` | 优先窗口是否被完整扫完。它排在最前，因此扫完它之后才出现的截断只影响上下文 |

两者都为真时，截断只记 `SCAN_CONTEXT_TRUNCATED`，不再置 `scanIncomplete`；无发现的结论保持 `NO_SYSTEM_EVIDENCE`（`confidence=medium`）而不是被压成 `INCONCLUSIVE`。截断发生在优先窗口内则 `priorityWindowComplete=false`，仍判 `INCONCLUSIVE`；优先窗口落在采集区间之外时按单段扫描，`priority` 为 false，结论保守。

契约影响：事件明细文件版本 `1` → `2`（新增两个布尔键），解析器接受 `1` 与 `2`，**旧样本仍可回放**，缺失的键取默认值 false，版本超范围时报明确错误。`OSGameDiagnosis.json` 的 `schemaVersion` 保持 `1`：只在 `observability` 下新增两个布尔字段，既有字段语义未变，launcher 只读 `verdict` / `confidence` / `userMessage` / `actions`，不受影响。

`BuildEventScanOrder()` / `IsEmptyEventScanRange()` 在 `EventLogReader.h` 导出，`EnvironmentCollectorTests` 覆盖：三段切分与顺序、边界不重叠、`priority` 标记唯一、无事故时间时退回单段、优先窗口落在区间外时退回单段且不冒充事故窗口、只截断上下文时维持 `NO_SYSTEM_EVIDENCE` 且记 `SCAN_CONTEXT_TRUNCATED`、事故窗口被截断时仍 `INCONCLUSIVE`、版本 1 事件文件回放后新字段取默认值。

### 事故锚点与 isTargetGame

采集包要能被归因，必须同时有 `incidentTimeUtc` / `incidentKind` / `gameExecutable`；缺任一项就既不能证明"是游戏导致的"，也不能证明"与游戏无关"。缺任一项时：

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

`null` 与 `false` 必须分开："从来没告诉我们是哪个游戏"和"确定不是那个游戏"是两个答案，压成一个会让研发把"没填"读成"不是游戏"。文件头另有 `gameExecutableSpecified`。

`module` 只在**该文件已经点名了这个进程**时才可读（指定的游戏、或 `dwm.exe`）：Application Error 的 `ModuleName` 是独立于 `AppName` 取的一个字段，而崩溃发生在 exe 自身时这个模块名往往就是那个进程名——`application` 解析不出来时若保留 `module`，等于把 `OTHER_PROCESS` 想藏的名字原样交出去。其余一律 `REDACTED`。

`EnvironmentCollectorTests` 覆盖：三项全空 / 三项齐全 / 只缺 kind 三种组合的判定、齐全时不输出警告、`manifest` 字段内容。

### 报告文件的发布方式（launcher 异步读取契约）

launcher 不等工具跑完再读结果，而是工具还在跑时就轮询输出目录。所以所有报告文件（`OSGameDiagnosis.json`，以及 `--collect` 各阶段的 `*.json` / `summary.txt`）都**先写临时文件、再 `MoveFileEx(REPLACE_EXISTING)` 改名发布**（`ReportWriter.cpp` 的 `WriteReportFile`）：

- 临时文件是同目录下的 `<目标名>.<pid>.tmp`，无论发布成功还是失败都不会留下它；读者只会看到"旧的完整文件"或"新的完整文件"，不会看到被截断的半个 JSON。
- 发布需要目标文件的 DELETE 权限，而 MSVC 的 `std::ifstream` 只共享读和写、**不共享删除**：读者一直开着文件不放会让发布失败。工具重试约 1 秒再放弃，放弃时抛异常且不留下半成品，等价于"本次没有产出报告"——launcher 应按"没检查过"处理（fail-open）。**launcher 读报告时请用 `FILE_SHARE_DELETE` 打开，读完立刻关闭。**
- 覆盖已有报告的行为不变（每次运行都重写）；与 ZIP 相反，`ReportArchive.cpp` 的 `MoveFileW` 故意不允许覆盖已发布的证据包。

### --preflight：每次拉起时的机器体检

```
ZEnvironmentCollector.exe -batchmode --preflight [--output-dir <path>] [--game-dir <path>]
                          [--min-spec <path>] [--game-exe <name>] [--budget-seconds N] [--quiet]
```

产出 `OSGamePreflight.json`，和诊断报告用同样的四个键：`verdict`、`confidence`、`userMessage`、`actions`；launcher 用同一套解析就能读两个文件，`"mode":"preflight"` 用来区分是哪一种。

| verdict | 含义 | launcher 应当 |
|---|---|---|
| `Pass` | 没发现值得说的 | 不提示 |
| `Advise` | 可提示的环境风险（见下） | 非模态提示，同一个提示只说一次 |
| `BelowMinimum` | 缺 AVX2、内存低于最低要求（默认 4 GB）、非 64 位 Windows、没有任何一张显卡能初始化 Direct3D 11、只有软件渲染器能初始化 D3D11（驱动没装）、功能级别低于 `minimumFeatureLevel` | 明确提示，但不阻断启动 |
| `Inconclusive` | 没跑完（超时或探针失败） | 当作"没检查过" |

`Advise` 覆盖：同厂商两张卡驱动版本不一致、选到集显、显示器接在集显上、只有软件渲染、可用内存过低、页面文件过小或未启用、未检测到 VC++ 运行库、游戏默认要用的显卡无法初始化 D3D11（另一张可以）、显存低于游戏要求、游戏盘或系统盘可用空间不足、有更新或驱动装完在等重启、很久没重启、最近显卡驱动崩溃重置过、游戏装在机械硬盘上、装在 U 盘/SD 卡/网络盘或非 NTFS 分区、安装路径含非 ASCII 字符（系统代码页非 UTF-8 时）、预览版系统。

取舍：

- **限时**：默认最多 3 秒（`--budget-seconds` 可设 1–30），到点按 `Inconclusive` 结束，不会把卡住的进程留在玩家机器上。
- **同时只跑一个**：已有实例在跑时新的直接退出且不写文件，免得慢的那个把新结论覆盖成过期的（即上面的单实例限制）。
- **只做快速只读检查**：CPU、已装与可用内存、页面文件、系统运行时间、系统版本、显卡选卡、每张卡接了几个显示器、机箱类型（区分台式机与笔记本）、VC++ 运行库是否在位、游戏卷与系统卷的可用空间（后者仅在与游戏卷不同盘时单独判）。不扫盘、不加载游戏 DLL、不写注册表（只读）、不提权。**唯一例外是事件日志**：下面"最近显卡驱动崩溃重置过"那条，它是一个有独立截止时间的窄查询，不是完整采集那种回看两周的全量扫描。
- **"内存不足"看两个不同的数**：已装内存低于最低要求算 `BelowMinimum`；已装够但**当前可用**很少只算 `Advise`——闪退报告里最常见的正是"16 GB 机器只剩几百 MB"，而"已装 16 GB"这条规则对此一言不发。
- **"未检测到 VC++ 运行库"只提示不阻断**：游戏是 /MT 还是 /MD 链接、运行库装在系统里还是随包放在游戏目录里，从进程外都看不出来。两种在位方式都算有，判断不出来时不提示。
- **"显示器接在集显上"只在台式机上提示**：笔记本双显卡切换机型本来就是由集显输出画面，独显没接显示器是正常状态。
- **D3D11 是这里唯一"起不来"级别的显卡检查**，也是唯一会启动显卡驱动的检查。Windows 客户端只跑 D3D11（`ProjectSettings.asset` 里 `WindowsStandaloneSupport` 的 `m_APIs: 02000000` 按 `GraphicsDeviceType` 枚举就是 `Direct3D11 = 2`，且 `m_Automatic: 0`），所以"这台机器能不能初始化 D3D11"决定游戏能不能启动——这是"双击没反应"里最硬的一类原因。五条刻意取舍：
  - **必须建设备才能问**：`IDXGIAdapter::CheckInterfaceSupport` 对 D3D11/12 在实测的每张卡上都返回 `DXGI_ERROR_UNSUPPORTED`。代价是几十毫秒（热 85–95 ms、冷最高 0.9 s），所以它在探针预算内跑，超时就 `Inconclusive`。
  - **钉在游戏会选的那张卡上**：引擎 `GfxDeviceSetup.cpp` 在没有 `-force-device-index` 时用 `EnumAdapterByGpuPreference(HIGH_PERFORMANCE)` 选卡并赋给 `g_D3D11Adapter`，这里是同一个调用。
  - **软件适配器只记录、不算数**：微软基本渲染驱动程序**能**成功创建 D3D11 设备并报 11_1，而引擎在只有它的时候**真的会启动**（跑 WARP）。所以"只有软件渲染器能建设备"（`D3D11_ONLY_SOFTWARE_ADAPTER`，驱动没装，游戏能起但帧率极差）与"一张都建不出"（`D3D11_DEVICE_UNAVAILABLE`）是两个结论，前者不会说"无法启动"。
  - **首选的卡不行时会去问其它卡**：只有一张都不行才判 `BelowMinimum`，另一张能行只 `Advise`——把能玩的人拦在门外是这里唯一无法挽回的错误。
  - **级别阶梯与引擎逐项一致**：`11_1 / 11_0 / 10_1 / 10_0`（同 `D3D11Context.cpp` 的 `GetSupportedFeatureLevels()`），所以"建不出设备"等价于"低于 10_0"，即引擎自己的下限；`minimumFeatureLevel` 只在游戏着色器另有要求时才需要写。`d3d11.dll` 是**运行时加载**而不是链接——硬链接会把它不存在变成进程起不来。
- **页面文件看两个数，第二个还有上限**：真正掐死进程的是 commit 上限（物理内存＋页面文件），不是物理内存。比例要求**不超过 `pageFileShareCapBytes`（默认 8 GB）**：Windows 不按内存线性放大系统托管的页面文件，196 GB 内存配 12 GB 页面文件是健康的。内存大到 `pageFileRuleMaxMemoryBytes`（默认 16 GB）以上、且**根本没设**页面文件时不提示——那是机主看得见的选择。
- **"有更新或驱动装完在等重启"是独立状态**：判据是 CBS `RebootPending`、Windows Update `RebootRequired`，以及 `PendingFileRenameOperations` 里**指向驱动目录的那几条**（`\DriverStore\`、`\System32\drivers\`）。最后一条只看驱动路径：这个值会跨重启存活陈旧条目，照单全收会让玩家白重启一次。
- **"最近显卡驱动崩溃重置过"是 preflight 里唯一读事件日志的规则**：只看近 7 天，只数 TDR（Display 4101 与 LiveKernelEvent 117/141/142），判据与完整采集的归因表**共用同一个函数**；倒序读取且有独立 600 ms 截止时间，被截断只会少算；两个通道都读不到就是 `known=false`，绝不当成"没发生过"。一次就算值得说——"游戏黑屏／卡住"的反馈大多就是它。
- **"很久没重启"量的是开机时长**：`GetTickCount64` 不把睡眠算进去，天天睡眠的机器不会被当成陈旧。默认 14 天（`maximumUptimeDays`，写 0 关掉），只提示。
- **"游戏装在哪里"也是环境**：除可用空间外还看卷**类型**（`GetDriveTypeW`：U 盘/SD 卡 `DRIVE_REMOVABLE`、网络盘 `DRIVE_REMOTE`）和**文件系统**（非 NTFS 提示）——前者是"文件可能在游戏跑着时消失"，后者是 FAT32 装不下 4 GB 以上文件、exFAT 没有 ACL。两个都读不到就不说。
- **安装路径含非 ASCII 单独一条**：路径对启动器是 UTF-8，对一个从没被告知编码的窄 `fopen` 就是乱码，而打不开的文件表现为"资源缺失"而不是"路径错了"。**系统 ANSI 代码页是 `65001` 时要闭嘴**（窄 API 按 UTF-8 解码，乱码这个前提不成立）。代码页从注册表 `HKLM\...\Nls\CodePage\ACP` 读（不是 `GetACP()`，那个答的是本进程）。**读不到仍然照旧提示**。报告里的路径走 `SanitizePath`，用户名不进报告。
- **显存阈值默认关着**（`GPU_VIDEO_MEMORY_BELOW_MINIMUM`，`minimumVramBytes`）：每张卡的 `dedicatedVideoMemoryBytes` 一直在采、一直在报，但**够不够只有游戏知道**，所以判的是"游戏实际会跑的那张卡"（`gpuSelection.preferredAdapter`），门槛写在 min-spec 里，**默认 `0`＝不判**。原因：DXGI 报的显存不够可信（**集显会虚报**），而且跑在核显上的笔记本显存小是设计如此。它只 `Advise` 不 `BelowMinimum`，软件渲染器另有一条自己的提示。
- **机械硬盘单独一条**（`GAME_VOLUME_SPINNING_DISK`）：判据是游戏卷上一次 `IOCTL_STORAGE_QUERY_PROPERTY`（`StorageDeviceSeekPenaltyProperty`）——要付寻道惩罚就是有磁头要移动，一次 `CreateFile` + 一次 IOCTL，不需要管理员、不扫盘。它不是故障只是慢，所以只提示、动作是"挪到 SSD"。USB 硬盘盒和 RAID 卷可能答错，卷不回答就是 `known=false`。
- **系统盘是另一个盘时，它的空间单独算一份**（`SYSTEM_VOLUME_LOW_FREE_SPACE`）：Windows 所在卷上放着页面文件、`%TEMP%`、着色器缓存和崩溃转储，游戏装在宽敞的 D: 上跟它一点关系都没有。判据是 `GetSystemWindowsDirectoryW` 所在卷的可用空间，门槛复用 `minimumFreeDiskSpaceBytes`，报告里单独给 `"systemVolume"`。**游戏卷和系统卷同盘时只报一次**，去重写在规则里而不是采集里，所以它也能被单测覆盖。
- **不判断驱动新不新**：只在同一厂商两张卡、且**两边版本都读到了**时才提示版本不一致。读不到版本不等于版本不一致。
- **输出目录默认 `%LOCALAPPDATA%\OSGame\EnvCheck`**，不是当前目录：launcher 拉起时当前目录是游戏安装目录，往那里写会触发启动器自己的完整性扫描。
- **退出码只有 0 和 30**：0 = 结论已写入，30 = 工具自身出错。"查出问题"不算失败，结论放在文件里，所以 `exe && next` 不会被打断，launcher 一律读文件。

### 最低配置不由工具决定：min-spec.json

最低配置是**游戏**的属性，不是采集工具的属性，所以不编译进 exe，而是随游戏发布、每次拉起时读取：

```
<游戏目录>\min-spec.json        # 默认位置，可用 --min-spec <path> 覆盖
```

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

- **只认这一种形状**：一个扁平对象，值是布尔、整数，或 `minimumFeatureLevel` 这一项的名称字符串。嵌套、数组、其它键上的字符串一律拒绝。
- **没写的键沿用内置默认**：写两个键就是覆盖两个键，不是重写整份最低配置。
- **`0` 是关掉这条规则**，不是"要求 0"。
- **解析失败一律回落**：文件不存在、打不开、写坏了都用内置默认继续跑，并把原因写进报告的 `notes`。一次启动绝不会因为一个配置文件被打断。
- **`minimumFeatureLevel` 是唯一用文本写的阈值**（`"11_0"`、`"10_1"`…），写成裸数字会被拒；删掉这一行＝不限制功能级别。
- **认不出的键会被点名**：`"minimumGpuClockMhz"` 这种新键会被跳过并记进 `notes`，其余键照常生效——否则键名改了、阈值其实没生效，报告上完全看不出来。
- 报告里有 `"requirements"`（本次判定用的全部阈值）和 `"requirementsSource"`（`default` 或 `file:<路径>`）。玩家拿着一份"Pass"来投诉时，这两个键是唯一的答案：到底是机器没问题，还是给工具的就是这么一份配置。

### 玩家操作

解压完整工具包，运行 `ZEnvironmentCollector.exe`，填事故三要素（问题类型、可选填带时区的发生时间）、可选填**游戏目录**（`--game-dir` 的同义输入框，游戏 DLL 所在处），点"开始采集并生成 ZIP"，一次拿到全部结果：CPU、内存、BIOS/整机/主板型号、SMBIOS 内存配置、原始微码注册表字节、显卡/驱动、双显卡选卡验证（`gpu-selection.json`）、磁盘容量、事件诊断、WHEA 及已有内存诊断结果、**DLL 静态检查与隔离加载探针（`compatibility.json`）**，并在项目配置了日志目录时附带最新的游戏日志副本。默认输出到桌面 `EnvironmentReports`。

- 不读取序列号、不运行内存测试、不重启、不改 BIOS。
- 每个采集阶段在独立受 Job 管理的子进程执行，默认 45 秒超时；失败/取消保留已完成信息。系统权限不足、字段缺失、事件覆盖不完整会记录在报告中，不代表硬件健康。
- GUI 默认勾选"同意附带事件明细"，可手动取消；CLI 仍需显式 `--include-events` 才附带脱敏事件明细。
- 窗口右侧有可拖动滚动条，也支持滚轮或点击轨道翻页；滑块与内容位置同步，尺寸随 DPI 缩放。完成后点"打开目录"，确认内容后手动发送 ZIP；不自动上传。
- `compatibility` 阶段会在隔离子进程里加载 `GameCore.dll` / `GameCore_Standalone.dll`（`ZDllLoadProbe.exe`，无 CRT、无 AVX，进程外执行），所以这一步**不再只读**。`--game-dir` 不给时，扫描根只有工具自身目录（通常没有待测 DLL，探针也就没有目标）。
- 扫描结果与体积有上限：最多 64 个 DLL（`MaxDllCheckFiles`），成文后超过 8MiB 就从**列表尾部**丢弃重建，两种情况都记 `INCOMPLETE` 并在 `manifest` 里体现 `partial`。`GameCore` 永远排在最前——被上限丢掉的绝不能是它。归档单文件上限 16MiB，超限会导致整个包打包失败。该阶段耗时与其它阶段不同量级，超时下限 180 秒（`--stage-timeout-seconds` 可调）。

微码仅保存 `Update Revision` 原始字节，不声称跨 CPU 统一解码或应用桌面 Intel 门槛。内存标称/配置速率不等于 XMP 判定。BIOS 日期不是安装日期。没有 WHEA 或内存检测结果不等于通过检测。

### 游戏日志打包（gamelogs 阶段）

环境报告只有系统侧证据时往往看不出游戏自己打印了什么，因此 `gamelogs` 阶段把游戏日志目录里**窗口内最新的** `.log` / `.logcompr` / `.txt` 复制进报告目录并打进 ZIP，索引写入 `game-logs.json`。`.logcompr` 是游戏在 `Cache\Logs` 下写的压缩会话日志，漏掉它就等于没有游戏自己的记录。

- 目录由 profile 声明，不猜测：`logRoot`（`localLow` 默认，另支持 `localAppData` / `roaming` / `programData`）+ `logSubPath`（相对路径，禁止绝对路径、`..`、驱动器号）。OSGame 配置为 `logRoot=localLow` + `logSubPath=Tencent\osgameCNOfficial\Cache\Logs`。未配置 `logSubPath` 的 profile 不运行该阶段，manifest 也不会出现它。CLI 可用 `--log-dir <绝对路径>` 覆盖。
- 挑选顺序：先取事故时间 ±窗口内写过的日志，其余按最后写入时间倒序；上限 8 个文件、单文件 16MiB、合计 48MiB，超限文件不截断、只记 `skipped`（`TOO_LARGE` / `TOTAL_LIMIT` / `UNREADABLE` / **`FILE_LIMIT`**），避免把半个日志伪装成完整日志。
- 只复制不修改：以 `FILE_SHARE_READ|WRITE|DELETE` 打开，游戏仍在写也能读；**不跟随目录/文件链接**（重解析点在递归之前就排除）——日志目录里一个指向别处的 Junction 会把整台机器的日志收进一份要发出去的报告。副本命名 `gamelog-NN-<原名>.log`（非白名单字符替换为 `_`），原文件不改名、不删除、不截断。
- 不递归到底：只扫描日志目录及其下一层子目录（深度上限 3 层由 `MaxGameLogScanDepth` 控制，当前只走一层）。
- 索引记录每个副本的原始相对路径、字节数、修改时间、SHA256 与 `complete`（复制期间被追加时为 false）：写完后再问一次原文件大小，比复制到的更长就说明**尾巴没进来**——而日志的尾巴正是崩溃出现的地方。日志目录路径用 `SanitizePath` 去掉用户名。
- 目录不存在或窗口内没有日志时返回 `NO_LOGS`（manifest 中体现），**不计入 `partial`**；`game-logs.json` 仍然写入并给出 `reason`（`NOT_CONFIGURED` / `LOG_DIRECTORY_MISSING` / `NO_LOG_FILES_IN_WINDOW` / `NO_LOG_FILES_COLLECTED`）。
- 隐私：日志可能含账号、聊天等信息。副本只留在本地报告目录，不自动上传；`summary.txt` 在包含日志副本时追加提示，发送前需人工确认。
- `EnvironmentCollectorTests` 覆盖：只收 `.log`/`.txt`、名字白名单化、副本真实落盘、缺目录与空目录返回 `NO_LOGS`、profile 路径解析、**Junction 不被跟进**、**数量上限丢掉的日志会记 `FILE_LIMIT`**、**复制没拿到尾巴时 `complete` 为 false**（这条用导出的 `CopyLogFile()` 单测，用小于实际大小的值去调，同一套算术且不需要靠竞态线程去撞窗口）。

### CLI 与配置

```powershell
.\ZEnvironmentCollector.exe -batchmode --collect --profile .\profiles\osgame.ini --output-dir .\Reports --include-events
.\ZEnvironmentCollector.exe -batchmode --diagnose --diagnose-detail --incident-time 2026-10-12T18:30:00+08:00 --incident-kind crash --output-dir .\Diagnosis
.\ZEnvironmentCollector.exe -batchmode --diagnose --diagnose-from-events .\Diagnosis\OSGameDiagnosisEvents.tsv --output-dir .\Replay
.\ZEnvironmentCollector.exe -batchmode --gpu-selection
.\ZEnvironmentCollector.exe --profile .\profiles\osgame.ini          # GUI，指定 profile
.\ZEnvironmentCollector.exe --lang zh-CN                             # 界面语言（默认英文）
```

`--collect` 还支持 `--incident-time`、`--incident-kind crash|hang|unknown`、`--lookback-hours 1..720`、`--stage-timeout-seconds 1..300`、`--log-dir <游戏日志目录>`（覆盖 profile 配置）；未填事故时间按已有事件查找，不伪装成当前时间。退出 0 表示 ZIP 生成，完整性见 manifest；30 为错误。

`--gpu-selection` 打印离线回归用例与本机真实探测：回归全部通过退 0，任一用例选错退 1（说明 `GfxDeviceSetup.cpp` 的选卡逻辑回退了），工具本身异常退 30。本机不支持 `EnumAdapterByGpuPreference` 不算失败，只在文本里说明，仍退 0。

`--collect-stage` 与 `--wer-config` 是内部入口：采集器会以受 Job 管理的子进程、或提权 `ShellExecute` 的方式重新启动自己（`ToolExecutablePath()`，即同目录的 `ZEnvironmentCollector.exe`）。它们即使不带 `-batchmode` 也不会弹窗，玩家不要直接调用。

GUI 默认读取同目录 `profiles/default.ini`；其他项目只需设置 `[project]` 下的 `id`、`displayName`、`gameExecutable`、`lookbackHours`，需要附带游戏日志时再加 `logRoot`（默认 `localLow`）与 `logSubPath`。配置支持 ASCII 或 UTF-16LE BOM 编码，项目 ID 与进程名仅允许安全文件名字符。无目标游戏时使用 generic，不能把其它进程推断成目标游戏。

普通 CLI 并不是安全采集入口：`--check-dll` 会追加路径而不是关闭递归扫描。玩家用 GUI 或 `--collect`。

### 推荐 Windows 系统转储（WER）与已有 dump 导入

高级区域默认选择"Windows 系统崩溃转储（推荐）"，另保留"ProcDump 附加（需允许调试）"。WER 不用选择或运行 ProcDump：选择当前项目游戏 exe → 点击开始 → 确认完整内存隐私及注册表修改 → 批准 UAC → 界面提示系统捕获已启用后，正常启动游戏并复现崩溃。默认等 1 小时。目标需在当前用户会话运行；不同用户的游戏不会自动放宽输出目录权限。

**一次启用可连续收集多次崩溃**：启用后配置一直生效，每次崩溃各自复制一份（第一份 `crash.dmp`，之后 `crash-2.dmp`、`crash-3.dmp`…，单次会话上限 8 份），点击"安全停止捕获"、等待超时或达到上限后才统一恢复注册表配置。每份 dump 独立做完整内存/异常线程/PID/模块路径校验，晚到的 dump 不会被当作重复文件跳过。

只写 64 位注册表视图的 `HKLM/SOFTWARE/Microsoft/Windows/Windows Error Reporting/LocalDumps/<exe文件名>` 三值：DumpType DWORD=2、DumpCount DWORD=2、DumpFolder REG_EXPAND_SZ=本次私有目录。WER 按文件名匹配而非完整路径，因此其他同名程序也可能生成文件；采集器必须结合观测到的 PID、创建时间、dump 异常时间和完整模块路径才能自动接收。仅做可选的有限查询，不申请 VM_READ、不调试附加、不修改反作弊。进程保护导致身份证据不足时保留原文件，提示手工确认导入，不把任意新文件当目标崩溃。

提权动作仅在用户启用/恢复 WER 时发生，调用随包 CLI 的严格 `--wer-config` 内部入口，参数不进入 shell 执行。备份写入 `HKLM/SOFTWARE/ZEngine/EnvironmentCollector/WerBackupsV1/<exe>`，ACL 只允许管理员/SYSTEM 修改，普通用户只读。先持久化原值的存在性、类型、原始字节与预期新值，再写目标值；异常退出后可用"恢复当前项目 WER 原配置"重试。恢复逐值比较，若其他工具已改动，返回冲突并保留备份，不覆盖它们。空备份/部分应用均有恢复路径；不直接删整个 `LocalDumps` 键。**恢复是恢复先前配置，不是关闭已有全局 WER。** UAC 拒绝/恢复冲突时明确提示，窗口退出前对未恢复配置再次告警。

结束收集（用户停止/等待超时/达到份数上限）后按原配置恢复（可能再次弹 UAC），再补采环境、计算 SHA256 和 ZIP64 压缩。多份 dump 全部打进同一个 `capture.zip`，`checksums.json` 逐份记录 SHA256；`capture.json` 新增 `capturedDumpCount` 与 `dumps` 数组逐份记录 PID、异常码、dump 时间，顶层单份字段仍指向第一份以兼容旧读取器。用户主动停止且已收集到 dump 时按正常完成处理；一份都没收到时仍为取消/超时失败。停止标志只用于结束等待，不再传给后处理。会话目录为 `EnvironmentReports/crash-capture-wer-<GUID>`，其 `system-dumps` 保留 Windows 原文件，验证后的副本为 `crash.dmp`，最终分享包为该私有目录内的 `capture.zip`，不把完整内存包放到可能共享的父目录。目录权限限当前用户、管理员和 SYSTEM；不跟随目录链接，不覆盖已有文件。打开捕获结果按钮直接选中 ZIP。空间不足/超时/后处理失败生成仅含 `capture.json` 的 `capture-failure.zip`，不冒称含 dump。停止不会终止游戏、WER 或删除仍在写入的文件；晚到文件仍可手工导入。完整 dump 仍可能数 GB，至少 4GiB 起步，复制前要求额外约 2 倍 dump 大小 + 1GiB；单 dump 上限 64GiB。

"选择已有 dump 并打包"不改注册表、不运行 ProcDump，不删除或移动原文件；需先选择对应游戏 exe 并确认来源和完整内存隐私。验证完整内存/异常线程/PID/模块路径后复制、补采与打包。报告记录 `captureBackend=dump_import` 和 `identity=USER_CONFIRMED_IMPORT_AND_MODULE_PATH`，不伪称属于本次运行。ProcDump 与 WER 共用打包流程，最终包都在本次目录内的 `capture.zip`。WER/import 报告 schemaVersion=2，记录配置/恢复状态、身份依据、dump 时间与异常码；ProcDump 原 schema 不变。

限制：真实 WXQ.exe 及玩家反作弊环境尚未验证；自定义崩溃处理、自动调试设置或系统策略可能阻止 WER，配置成功不保证生成 dump，不用于卡死/普通退出。**跨用户目标、标准用户 UAC 拒绝、异机 ACL/受控文件夹三项仍未验收**（本机单会话且已提权，构造不出来）；当前只支持安全 exe 文件名、本地普通目录，文件路径映射不同的历史 dump 需匹配原游戏路径。工具未数字签名、不自动上传。参考：<https://learn.microsoft.com/en-us/windows/win32/wer/collecting-user-mode-dumps>。

验收记录（本机、已提权会话，2026-10）：HKLM 真路径走通了完整一圈——启用 → 真实连续捕获两份 → 恢复，`LocalDumps` 与备份键均无残留；备份键 ACL 实测为 `SYSTEM`/`Administrators` FullControl + `BUILTIN\Users` 只读，与上面的声明一致。手工验收命令 `EnvironmentWerTests.exe --consent-system-test normal <目录>`（未挂进 ctest，需手工跑），**前提是与 `ZEnvironmentCollector.exe` 同目录**：提权重启的是同目录的自己，缺了它根本不会弹 UAC，直接报"同目录缺少 `ZEnvironmentCollector.exe`"——这不等于 UAC 被取消，报后者时才该去批准提示。

### 高级诊断：一次完整崩溃 dump

普通报告与捕获结果分别保存。"打开捕获结果（定位 ZIP 或 capture.json）"只使用本窗口最近一次捕获结果，直接在资源管理器选中 ZIP 或 capture.json，不受普通采集影响。高级区域显示版本号。若重开窗口无历史结果，提示到桌面 `EnvironmentReports/crash-capture-*` 查找，不跳转普通报告冒充。

用户打开 GUI 并滚动到"高级诊断"，选择微软官方 `procdump64.exe` 和当前 profile 对应的游戏 exe（不是 Launcher）。点击开始后需分别确认完整内存隐私和 Sysinternals 许可；选择否不会启动监控。ProcDump 需从 <https://learn.microsoft.com/en-us/sysinternals/downloads/procdump> 自行下载，采集器只提供打开官网入口，不捆绑或镜像分发。通过签名/原始文件名/64 位 PE 检查才运行，并保持只读文件句柄防止校验后修改。证书在线验证不可用时拒绝运行，不降级跳过。当前微软页面列 ProcDump 支持 Windows 11 及以上，此模式不承诺 Windows 10 兼容。

- 目标约束：名称匹配 profile、完整路径匹配用户选择、同一 Windows 会话、单个 64 位实例；确认无已有调试器/ProcDump。以 PID 调用 `-accepteula -ma -e -n 1`，不使用全局 AeDebug 注册，不注入游戏、不主动启动游戏。默认等待及监控合计 1 小时，正常退出或未捕获异常时只保存失败/无 dump 清单，不假报成功；游戏重启后需再次点击开始。
- 空间：预检至少 4GiB，且不低于预计已提交内存两倍加 2GiB，监控中持续检查 1GiB 安全余量。完整 dump 最大 64GiB；空间不足拒绝启动或请求安全停止。完整 dump 可能包含账号/聊天/令牌和完整路径，无法脱敏；仅本地保存并经安全渠道手动发送，ZIP 本身未加密。
- 停止：只通过微软支持的 `ProcDump-<目标PID>` 事件请求优雅退出并等待恢复目标。该事件影响同目标的所有 ProcDump 实例，因此启动前拒绝已有监控。绝不把现有 helper 的 KillOnJobClose/TerminateProcess 用于 ProcDump。普通关闭窗口等待安全退出；异常清理也走同一保护，若安全入口不可用会等待而不是强杀。不能保证用户强杀采集器、操作系统关机等场景仍可完成取证。
- 完成判定：等待 ProcDump 退出并关闭输出文件；联合完成文本、支持的退出码、MDMP 头、FullMemory 标志、Memory64 负载范围、Exception/Thread/MiscInfo、目标 PID 验证，再标记 dump 有效。官方 12.01 实测重定向输出为 UTF-16LE、成功一份 dump 返回 1，因此不能按普通 exit 0 规则判断。其他输出格式未知时保留原始文件并报告未确认，不猜成功。
- 结果包：`crash.dmp`、`environment.zip`、`capture.json`、`procdump.log`、`checksums.json`。环境子包沿用原白名单；清单保存同意时刻、目标名/PID、EXE 文件版本和 SHA256、ProcDump 版本及 SHA256、异常码、dump 时间/大小、验证结果。EXE 文件版本不保证等同游戏资源版本，定位仍需结合 dump 模块及发布记录获取匹配 PDB。原始 dump 始终保留；后处理取消/失败不删除它。
- 日志/哈希：ProcDump 日志保存最多 1MiB 输出并转 UTF-8。成功后所有证据文件计算 SHA256，按白名单流式 Deflate 压缩成 ZIP64 并实际解压回读 CRC。64KiB 文件读缓冲，不把数 GB 文件装入内存。大 dump 打包可取消；环境子包的短暂最终阶段维持原不可取消行为。
- 设置：采集器将成功完成签名检查后的 ProcDump 路径记在 `HKCU/Software/ZEngine/EnvironmentCollector/ProcDumpPath`；每次 GUI 仍需重新确认许可与内存采集，偏好写失败不丢取证结果。没有修改全局调试器注册和反作弊设置。
- 压缩依赖：`engine/3rdparty/minizip-ng`（主仓子模块），以 deflate 单方法构建，ZIP64 由库原生支持，无补丁、无本地修改。交付源码须包含 `LICENSE-minizip-ng.txt`（安装时从子模块 LICENSE 改名复制）。归档仍为流式：64KiB 读缓冲，写完回读每个条目完整内容并由库比对 CRC32。

签名前置失败继续生成 `verification.zip`；签名后的进程访问/监控/后处理异常生成 `capture-failure.zip`，二者仅含 `capture.json`，不混入 dump 或原始日志。收集根目录不可写、授权前参数失败、取消/正常退出等仍可能只有本地 JSON 或没有新目录，不能承诺任何失败都必有 ZIP；磁盘打包失败明确提示发送 JSON。报告补充 `failedStage`、`failedOperation`、Win32 错误码、目标 PID 和 `requestedProcessAccess`；OpenProcess 失败、进程已退出、路径变化、已有调试器分别提示。不自动提权、不调整真实目标 DACL、不启用调试特权、不跳过签名或反作弊。截图上的"目标进程已变化或访问被拒绝"只证明本次已通过签名且未附加；不能由截图确认错误码 5 或断言反作弊是原因。

签名前置失败自动保存仅含 `capture.json` 的 `verification.zip`，保留 WinVerifyTrust 直接返回的 LONG 及十六进制值、错误分类、签名者/颁发者、文件 SHA256/版本、策略和 `procDumpStarted`。点击"打开 dump 取证目录"发送此包，不要重复发送顶部普通 `osgame-*.zip`。失败 ZIP 不是 dump，也不表示捕获成功；未通过验证不会启动 ProcDump，不会记住该文件。
