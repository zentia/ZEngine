# Windows Environment Check

`ZEnvironmentCollector` 是独立的 Windows x64 环境检查与采集工具，用于在不启动 Unity 客户端的情况下收集硬件信息、检查 DLL 构建配置，并通过隔离子进程验证 `GameCore.dll` 能否加载。

## 一个程序，两种用法

以前这里有三个可执行文件（采集器 GUI、`ZGpuSelectionInspector`、CLI `ZWindowsEnvironmentCheck`），现在合并成一个 `ZEnvironmentCollector.exe`：

```powershell
.\ZEnvironmentCollector.exe                    # 图形界面，三个页签
.\ZEnvironmentCollector.exe -batchmode --help  # 命令行，无窗口
```

`-batchmode` 沿用 Unity/UE 的写法：不再弹窗，结果写 stdout，退出码交给调用方。它必须成对出现——GUI 模式只认识 `--profile` / `--output-dir` / `--incident-time` / `--tab`，其它参数会直接报错提示加上 `-batchmode`。

程序是 **WINDOWS 子系统**（`wWinMain`），所以双击运行时加载器根本不会创建控制台，没有黑窗口一闪。以前是 CONSOLE 子系统 + 第一行 `FreeConsole()`，但那个控制台在任何代码执行前就已经存在并被显示过了，`FreeConsole()` 只能在它闪完之后把它关掉。

改子系统没有牺牲命令行：`-batchmode` / `--help` / 内部 worker 动词在打印任何东西之前会 `AttachConsole(ATTACH_PARENT_PROCESS)` 并把 CRT 的 stdin/stdout/stderr 重新指向调用方的控制台，所以 cmd.exe 与 PowerShell 下 stdout/stderr 照常、`%ERRORLEVEL%` 与 `$LASTEXITCODE` 照常。调用方如果已经把 stdout 重定向到管道或文件（`>`、`|`、CI 捕获），就保持不动，不去抢它的输出。cmd.exe 在 bat 里依然会等它结束，`exe && next` 仍然可用（已用"bat 里跑慢命令、下一行检查产物"实测，重定向与不重定向两种都等）。

GUI 三个页签与 `-batchmode` 子命令一一对应：

| 页签 | 对应命令 | 说明 |
|---|---|---|
| 环境采集 | `-batchmode --collect` | 只读采集 + ZIP，含崩溃捕获高级区 |
| 兼容性检查 | `-batchmode`（默认命令） | 硬件快照、双显卡选卡验证、DLL 静态检查与隔离加载探针 |
| GPU 选卡验证 | `-batchmode --gpu-selection` | 离线回归用例 + 本机真实探测 |

GUI 的“兼容性检查”和“GPU 选卡验证”与命令行走的是同一份代码（`EnvironmentCheckCommand` / `GpuSelectionProbe`），不会出现两套结果。

## 界面语言

**默认是英文**，界面右上角的语言下拉框可切到简体中文，选择按用户记在注册表
`HKCU\Software\ZEngine\EnvironmentCollector\Language`（和 ProcDump 路径同一个键），
下次启动自动沿用。临时覆盖：`ZEnvironmentCollector.exe --lang zh-CN`（`en` | `zh-CN`）。

实现约定（多语言**机制**在 ZSlate，**数据**在引擎的 DataTable）：

- 机制在 `ZSlate/Core/SlateLocalization.h`：英文写在源码里（"id" + 英文原文），
  其它语言是注册表式词条表；缺词条自动回退英文，并在日志里报一次缺失的 id。
- 词条**不是硬编码**的：简体中文写在 `resource/ui_strings_zh-CN.csv`，构建期由
  `DataTableCook` 烤成 `resource/ui_strings_zh-CN.zasset`（真正的引擎 DataTable，行类型见
  `Runtime/Resource/Asset/Data/LocalizationTable.h`），运行时通过
  `Runtime/ZEngineApi/ZEngineToolApi.h` 的 C ABI 读取。改文案是改 CSV，不是改 C++。
- `UiLocalization.cpp` 只负责“把表读进来 + 读写语言偏好”，它不依赖 ZSlate，
  CLI-only 构建也会链接它。
- 切换语言会重建控件树（字符串是在建控件时解析的，不是绘制时），表单里已填的内容会被带过去。
- **报告文件内容（TXT/JSON）不受语言开关影响，始终中文**：那是给研发和支持的交付物，
  不是给玩家看的界面。

## 与 ZRuntimeShared 的关系

全树构建时本工具链接 `ZRuntimeShared.dll`，日志和多语言表都来自引擎——原因是这两件事
引擎本来就有，而本工具正好都需要：

- **日志**：引擎的 `LogSystem`（bqlog）。工具是 WINDOWS 子系统进程，双击启动时 stderr
  没人看得见，而它要报告的失败（缺字体、WER 恢复失败、ProcDump 没产出 dump）恰恰是玩家
  描述不出来的那些。日志落在 `%USERPROFILE%\AppData\LocalLow\ZEngine\Logs\ZEnvironmentCollector_*.log`。
- **多语言表**：硬编码在代码里不好维护，走表；而表本身是引擎能力（DataTable +
  AssetManager + SerializedFile），所以一并从引擎取。

两者都只通过 `extern "C"` 入口（`ZRuntime_*`）跨模块，不传 `std::string`、不跨模块
`new/delete`，因此 `/MT` 的 exe 也能安全消费 CRT 选择不同的 DLL。

只起引擎的**最小切片**（`RegisterCore()` + `RegisterPlatform()`）：反射、内存、FileSystem、
ObjectManager、AssetManager，加上 LogSystem。**不注册** Application / WindowSystem / 任何 RHI /
ScriptingManager / ProjectInfo——一个被派去诊断机器的工具，不该在那台机器上开窗口或建 GPU 设备。

构建方式随之改变：

| 配置方式 | 后端 | 说明 |
|---|---|---|
| 全树 `cmake -S . -B build` | 引擎后端（链接 ZRuntimeShared） | **分发用的构建**，配合 `-DZENGINE_STATIC_CRT=ON` 让 DLL 也是 `/MT`，产物仍能发到没有 VC redist 的机器（已验证：exe 与 DLL 都不再 import `MSVCP140`/`VCRUNTIME140`）。此前 Jolt 固定 `/MD`，会把 `ZRuntimeShared.dll` 的链接打断；现在它跟随该开关 |
| 独立 `cmake -S engine/tools/WindowsEnvironmentCheck -B <dir>` | 自包含（静态 bqlog + 内置表） | 没有 ZRuntimeShared 目标（那要拉起整个 Runtime 依赖树），退化为旧路径 |

注意：引擎后端下 `ZRuntimeShared.dll` 必须和 exe 同目录（POST_BUILD 已拷贝），
否则进程起不来。

## 产物

Release 构建后需要一起分发：

```text
ZEnvironmentCollector.exe
ZDllLoadProbe.exe
ZRuntimeShared.dll          <- 仅引擎后端（全树构建）
resource\ui_strings_zh-CN.zasset   <- 仅引擎后端，构建期由 DataTableCook 生成
profiles\default.ini
profiles\osgame.ini
LICENSE-Zydis.txt
LICENSE-Zycore.txt
```

- `ZEnvironmentCollector.exe`：主程序（GUI + `-batchmode` CLI）。
- `ZDllLoadProbe.exe`：无 C/C++ CRT、无 AVX/AVX2 的隔离 DLL 加载探针。
- `ZRuntimeShared.dll`：引擎切片（日志 + 表）。缺了它进程起不来，不只是少个功能。
  用 `-DZENGINE_STATIC_CRT=ON` 构建时它也是 `/MT`，所以目录照样是自包含的。
- `resource\ui_strings_zh-CN.zasset`：简体中文界面表。缺失时界面降级为英文，工具仍然可用
  （引擎日志里会写明原因）——诊断工具不能因为自己的资源目录坏了就跑不起来。
- `profiles\*.ini`：项目配置，GUI 默认读取同目录 `profiles/default.ini`。
- `LICENSE-*.txt`：静态链接的 Zydis/Zycore MIT 许可证。

不要单独分发主程序，否则动态探针会报告 `DLL_PROBE_INFRASTRUCTURE_FAILED`，`--collect` 的分阶段子进程也会报 `HELPER_MISSING`。

## 日志

工具自身的诊断走扁鹊（bqlog，分类 `ZEnvironmentCheck`），落盘到

```text
%USERPROFILE%\AppData\LocalLow\ZEngine\Logs\ZEnvironmentCollector_YYYYMMDD_N.log
```

与引擎日志同目录。GUI 是 WINDOWS 子系统进程、没有 stderr，字体缺失、D3D 初始化失败、后台任务报错等只能在那里看到（ZSlate 的字体/图集告警也一并转发进来）。`manifest.json` 里的 `toolLog` 字段记录该路径，日志本身不进 ZIP。

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
- 双显卡自动选卡验证：离线回归用例（4 个，与 `GfxDeviceSetup.cpp` 的回退启发式逐字节一致）+ 本机 `EnumAdapterByGpuPreference` 真实探测

选卡验证由 `CollectEnvironment()` 采集，是环境快照的一部分，因此 `-batchmode` 报告、GUI “GPU 选卡验证”页签读的是同一份数据；`--collect` 也作为独立阶段写入 `gpu-selection.json` 并打包进 ZIP。三处共用 `BuildGpuSelectionJson()`，不会出现两套结果。

### DLL 静态检查

工具递归扫描自身目录、当前工作目录及其子目录中的所有 `.dll`：

- PE 文件有效性
- x64 架构
- 导入库
- Debug CRT：`MSVCP*D.dll`、`VCRUNTIME*D.dll`、`ucrtbased.dll` 等
- 使用 Zydis 扫描 x64 Runtime Function 范围中的 AVX、AVX2、FMA、F16C、AVX-512 指令
- 输出指令 RVA、文件偏移、机器码及反汇编文本样例

静态包含 AVX/AVX2 不代表旧 CPU 一定执行该指令。受 CPUID 保护的优化路径允许存在，因此静态扫描只提供风险信息，不直接判定加载失败。

### DLL 动态探针

以下文件会自动进入隔离动态探针：

```text
GameCore.dll
GameCore_Standalone.dll
```

也可以通过 `--probe-dll` 显式指定。探针执行：

1. 在独立进程中调用 `LoadLibraryExW`。
2. 对 GameCore 默认查找并调用 `OSG_LocalPredictHardwareCapable()`。
3. 捕获非法指令、访问违规、栈溢出和 DLL 初始化异常。
4. 将 Win32 错误、异常码、导出函数返回值和 runner 输出写入报告。

## 基本使用

将工具与待测 DLL 放在同一目录，直接运行：

```powershell
.\ZEnvironmentCollector.exe -batchmode --output-dir .\EnvironmentReport
```

默认支持无 AVX 的旧 CPU。如果产品明确要求某些指令集，可以配置：

```powershell
.\ZEnvironmentCollector.exe -batchmode `
  --require avx,avx2,fma,f16c `
  --min-memory-gb 8 `
  --output-dir .\EnvironmentReport
```

只检查指定 DLL：

```powershell
.\ZEnvironmentCollector.exe -batchmode `
  --check-dll .\GameCore.dll `
  --output-dir .\EnvironmentReport
```

显式执行隔离加载：

```powershell
.\ZEnvironmentCollector.exe -batchmode `
  --require none `
  --probe-dll .\GameCore.dll `
  --output-dir .\EnvironmentReport
```

只想看机器、不想等递归 DLL 扫描和隔离加载探针时加 `--no-dll-checks`（GUI 的“兼容性检查”页签上是同名复选框）。

## Intel SDE 模拟旧 CPU

Intel SDE 不随本工具分发，需要从 Intel 官方网站下载并接受其许可协议：

<https://www.intel.com/content/www/us/en/download/684897/intel-software-development-emulator.html>

模拟 Nehalem（例如 Intel Core i5-760）：

```powershell
.\ZEnvironmentCollector.exe -batchmode `
  --require none `
  --probe-runner "C:\Tools\IntelSDE\sde.exe" `
  --probe-runner-arg "-nhm" `
  --output-dir .\SdeReport
```

当前 SDE 中 `-nhm` 已同时配置 Nehalem CPUID 和 chip-check，不要额外传递无参数的 `-chip-check`。

若需要其他 CPU 模型，先查看：

```powershell
C:\Tools\IntelSDE\sde.exe -help
```

SDE 的 stdout/stderr 会写入报告中的 `runnerOutput`。关键字段：

```json
{
  "loadSucceeded": true,
  "exceptionCodeHex": "0x00000000",
  "runnerReportedIsaViolation": false,
  "runnerReportedInternalError": false,
  "exportCalled": true,
  "exportResult": 0
}
```

`OSG_LocalPredictHardwareCapable()` 在旧 CPU 上返回 `0` 表示本地预测优化不可用；只要 DLL 成功加载且没有非法指令，就不视为加载失败。

## 报告

工具生成：

```text
OSGameEnvironmentReport.txt
OSGameEnvironmentReport.json
```

当前 JSON 版本：

```json
{
  "schemaVersion": 5,
  "toolVersion": "1.8.0"
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
| `24` | 探针程序或 runner 基础设施不可用 |
| `30` | 主工具内部错误 |

常见 `issueCode`：

- `DLL_DEBUG_RUNTIME_UNSUPPORTED`
- `DLL_LOAD_FAILED`
- `DLL_PROBE_ILLEGAL_INSTRUCTION`
- `DLL_PROBE_EXCEPTION`
- `DLL_PROBE_TIMEOUT`
- `DLL_PROBE_EXPORT_NOT_FOUND`
- `DLL_PROBE_INFRASTRUCTURE_FAILED`

## 事件诊断与玩家一键采集（1.7.2）

GUI 使用 ZSlate，不依赖 ZRuntime、编辑器或游戏进程。公共源码只在本目录维护；OSGame 通过 profile 和发布适配接入，G侧已删除迁移完成的旧源码，仅保留分发适配和接口说明。

### 1.7.3 事件扫描顺序修复

1.7.2 及更早版本按时间正序扫描整个回溯区间，命中 `MaxRelevantEvents`（5000）上限后丢弃的是**最近的**事件，也就是最可能相关的那批；填了 `--incident-time` 也只是把区间向前**扩大**，没有锚定到事故时刻。现网实测：某台机器 14 天内有 4227 条 `Windows Error Reporting/1001 crashpad_log`，上限在事故前 6 天就被占满，`events-redacted.json` 只覆盖到事故前 6 天，事故当天一条都没扫到，却输出了 `NO_SYSTEM_EVIDENCE` 之外的误导结论——此时"没发现显卡/存储异常"是无效的否定证据。

1.7.3 的改动：

- 提供 `--incident-time` 时，`BuildEventScanOrder()` 把区间切成 3 段并按此顺序扫描：① `[事故-30min, 事故+30min]`（决定 verdict 的窗口）→ ② 事故之后（Windows 延迟写入的 bugcheck / WER）→ ③ 更早的上下文。上限或超时只会牺牲上下文，不再牺牲事故窗口。三段边界互不重叠（闭区间之间留 1 tick），不会重复计入同一条事件。
- 每段改用 `EvtQueryReverseDirection` 倒序读取，即使单段内命中上限，丢弃的也是最旧的记录。
- 两个通道（System / Application）都先扫优先窗口再扫上下文，因为同一条规则（如 `GPU_TDR`）的证据可能来自任一通道。
- 读取结束后把 `events` 与 `systemEventTimes` 重新按时间升序排序：规则本身与顺序无关，但事件文件要给人看、也要能回放。
- `SYSTEM_ISSUES_NEARBY` 的用户文案区分远近：证据不在事故时刻附近（无事故时间或 `proximity=lookback`）时不再说"可能与你遇到的问题有关"，改为明确"该记录不在问题发生的时间点附近，无法确定与本次问题有关"。此前一条 6 天前的 `Ntfs/98` 会让玩家以为硬盘坏了。
- `systemLogReadable` 维持原语义，只由区间查询的结果决定；读取最旧事件的探针失败或日志为空只影响 `systemOldestKnown`，不会被误判成"日志不可读"而直接 fatal。

- 区分“只截断了上下文”与“事故窗口被截断”。`budgetExceeded` / `eventCapReached` / `queryErrorOccurred` 是全局标志，无法说明截断是否真的损失了事故证据，因此 `Observability` 增加两个字段：

  | 字段 | 含义 |
  |---|---|
  | `priorityWindowRequested` | 本次是否请求了优先窗口（等价于“是否提供了事故时间”） |
  | `priorityWindowComplete` | 优先窗口是否被完整扫完。优先窗口排在最前，因此扫完它之后才出现的截断只影响上下文 |

  两者都为真时，截断只记 `SCAN_CONTEXT_TRUNCATED`，不再置 `scanIncomplete`；无发现的结论保持 `NO_SYSTEM_EVIDENCE`（`confidence=medium`）而不是被压成 `INCONCLUSIVE`。若截断发生在优先窗口内，`priorityWindowComplete=false`，仍按原逻辑判 `INCONCLUSIVE`。优先窗口落在采集区间之外时按单段扫描，`EventScanRange::priority` 为 false、`priorityWindowComplete` 保持 false，结论保守。

契约影响：

- 事件明细文件版本 `1` → `2`（新增两个 `O` 键）。解析器接受 `1` 与 `2`，**旧样本仍可回放**，缺失的键取默认值 false；版本超出范围时报明确错误而不是笼统的 header 错误。
- `OSGameDiagnosis.json` 的 `schemaVersion` **保持 1**：本次只在 `observability` 下新增上述两个布尔字段，既有字段语义未变。launcher 只读 `verdict` / `confidence` / `userMessage` / `actions`，不受影响；保持 1 可避免严格校验 `schemaVersion == 1` 的消费端被动失败。

`BuildEventScanOrder()` / `IsEmptyEventScanRange()` 在 `EventLogReader.h` 导出，`EnvironmentCollectorTests` 覆盖：三段切分与顺序、边界不重叠、`priority` 标记唯一、无事故时间时退回单段、优先窗口落在区间外时退回单段且不冒充事故窗口、只截断上下文时维持 `NO_SYSTEM_EVIDENCE` 且记 `SCAN_CONTEXT_TRUNCATED`、事故窗口被截断时仍 `INCONCLUSIVE`、版本 1 事件文件回放后新字段取默认值。

### 1.8.1 事故锚点与 isTargetGame

2026-10 连续三份采集包（r809053、#45665、Lenovo 82RC）都因为缺 `incidentTimeUtc` / `incidentKind` / `gameExecutable` 中的一项或多项而**无法归因**：包本身看起来是完整的，研发打开后才发现它既不能证明"是游戏导致的"，也不能证明"与游戏无关"。1.8.1 让工具自己把这件事说清楚，而不是留给收包的人。

**事故锚点**（`DescribeAttributionAnchor`）：三项齐全才算可归因。缺任一项时——

| 出口 | 行为 |
|---|---|
| `summary.txt` | **顶部**输出分隔线包裹的中文警告，逐项列出缺什么 |
| `manifest.json` | 新增 `"attributionUsable"` 与 `"missingAttributionInputs":[...]`，供分诊脚本在人工打开之前过滤 |
| GUI | 采集页在项目名下方常驻红色提示（改类型/填时间即时刷新），点「开始采集」时再弹一次确认 |
| CLI | `-batchmode --collect` 在 stdout 打印同一段警告 |

不阻断采集：纯机器体检本来就是合法用法，只是不能让它冒充具备归因价值。

**`events-redacted.json` 的 `isTargetGame`**（schemaVersion `1` → `2`）：此前崩溃进程名一律脱敏成 `OTHER_PROCESS`，外部研发拿到包后**连"是不是我们的游戏在崩"都判断不了**——而这是采集包存在的唯一理由。新增字段只回答这个布尔问题，不泄露进程名：

| `gameExecutableSpecified` | `isTargetGame` | 含义 |
|---|---|---|
| false | `null` | 没给游戏 exe，无法判定（**不是** false） |
| true | `true` / `false` | 等于 / 不等于指定的游戏 exe |

`null` 与 `false` 必须分开："从来没告诉我们是哪个游戏"和"确定不是那个游戏"是两个不同的答案，压成一个会让研发把"没填"读成"不是游戏"。文件头另有 `gameExecutableSpecified`，便于一眼看出属于哪种。

`EnvironmentCollectorTests` 覆盖：三项全空 / 三项齐全 / 只缺 kind 三种组合的判定、齐全时不输出警告、`manifest` 字段内容。

### 报告文件的发布方式（launcher 异步读取契约）

launcher 不等工具跑完再读结果，而是工具还在跑时就轮询输出目录。所以所有报告文件
（`OSGameDiagnosis.json`，以及 `--collect` 各阶段的 `*.json` / `summary.txt`）都改为
**先写临时文件、再 `MoveFileEx(REPLACE_EXISTING)` 改名发布**（`ReportWriter.cpp` 的
`WriteReportFile`）：

- 临时文件是同目录下的 `<目标名>.<pid>.tmp`；无论发布成功还是失败都不会留下它。
- 读者只会看到“旧的完整文件”或“新的完整文件”，不会看到被截断的半个 JSON。改之前是
  `ofstream(path, trunc)` 就地写，轮询方在 truncate 之后、写完之后之间会读到空文件。
  `EnvironmentCollectorTests` 用并发读线程覆盖了这个性质（换回旧实现该断言会失败）。
- 发布需要目标文件的 DELETE 权限，而 MSVC 的 `std::ifstream` 只共享读和写、**不共享删除**：
  读者一直开着文件不放会让发布失败。工具会重试约 1 秒再放弃，放弃时抛异常且不留下半成品，
  等价于“本次没有产出报告”——launcher 应按“没检查过”处理（fail-open）。
  **launcher 读报告时请用 `FILE_SHARE_DELETE` 打开，读完立刻关闭。**
- 覆盖已有报告的行为不变（每次运行都重写），与 ZIP 相反：`ReportArchive.cpp` 的
  `MoveFileW` 故意不允许覆盖已发布的证据包。

### --preflight：每次拉起时的机器体检（launcher 异步调用）

```
ZEnvironmentCollector.exe -batchmode --preflight [--output-dir <path>] [--game-dir <path>]
                          [--game-exe <name>] [--budget-seconds N] [--quiet]
```

产出 `OSGamePreflight.json`，**与 `OSGameDiagnosis.json` 同构**：`verdict` / `confidence` /
`userMessage{code,zh}` / `actions[{code,zh}]` 四个键同名、同形、同顺序，launcher 一套解析代码
读两个文件；`"mode":"preflight"` 用来区分。另有 `findings[]`、`budget{}`、`environment{}` 供
排查与取证，launcher 可以只读那四个键。

| verdict | 含义 | launcher 应当 |
|---|---|---|
| `Pass` | 没发现值得说的 | 不提示 |
| `Advise` | 可提示的环境风险：同厂商驱动版本不一致、选到集显、只有软件渲染、磁盘可用空间不足、预览版系统 | 非模态提示，同一 `code` 提示一次后记住 |
| `BelowMinimum` | 缺 AVX2、内存低于最低要求（默认 4 GB）、非 64 位 Windows | 明确提示，但**不阻断**——本方案 launcher 只提示 |
| `Inconclusive` | 没跑完（超时或探针失败） | 当作"没检查过"，fail-open |

- **硬超时**：探针跑在独立线程上，主线程最多等 `--budget-seconds`（默认 3，范围 1–30）。超时
  就 detach 该线程、发布 `Inconclusive` 并退出——绝不让一个卡在驱动 IOCTL 里的进程留在玩家机器上。
  超时时不采用半截的探针结果：半截的适配器列表读起来就是"这台机器没有显卡"。
- **单实例**：互斥体按输出目录取名；抢不到锁的实例**直接退出且不写文件**，否则慢的那个会把过期
  结论改名覆盖掉新的那个。
- **只做廉价只读的事**：CPUID / `GlobalMemoryStatusEx` / `RtlGetVersion` / DXGI 适配器与
  `EnumAdapterByGpuPreference` 选卡探测 / 游戏所在卷 `GetDiskFreeSpaceEx` / 读 `BuildLabEx`
  判预览版。**不做** DLL 递归扫描与 Zydis 反汇编、`LoadLibrary(GameCore.dll)` 隔离探针、
  `\\.\PhysicalDriveN` 全枚举、事件日志、任何注册表写入或提权。
- **不做"驱动过旧"判断**：OEM 驱动日期格式各异、移动卡驱动滞后是常态，只在同一厂商两张卡驱动
  版本不一致时才提示（这是 45665 那类混装的真因）。
- **输出目录默认 `%LOCALAPPDATA%\OSGame\EnvCheck`**，绝不是当前目录：launcher 拉起时 cwd 是
  游戏安装目录，往那里写新文件会触发启动器自己的完整性扫描/修复。
- **退出码只有 0 和 30**：0 = 结论已写入（或已有实例在跑、本次有意不写），30 = 工具自身错误。
  非零结论放在文件里而不是退出码里，这样 `exe && next` 不会被"查出问题"打断，launcher 一律读文件。
- 进程自身把优先级降到 `BELOW_NORMAL_PRIORITY_CLASS`；IO 优先级需要 launcher 在创建进程时指定。

`EnvironmentPreflightTests` 覆盖：四态判定、五条提示规则、"枚举失败"不等于"没有显卡"、未完成即
`Inconclusive`、四键契约文本、超时等待本身、单实例互斥（跨线程验证，同线程测这个没有意义）、
默认输出目录不是 cwd、端到端落盘且无临时文件残留。

### 玩家操作

解压完整工具包，运行 `ZEnvironmentCollector.exe`，顶部页签切到“环境采集”，选择问题类型、可选填写带时区的发生时间，点击“开始采集并生成 ZIP”。默认输出到桌面 `EnvironmentReports`。GUI默认勾选“同意附带事件明细”，可手动取消；CLI仍需显式传入 `--include-events` 才附带脱敏事件明细。窗口右侧提供可拖动滚动条，也可使用滚轮或点击轨道翻页；滑块与内容位置同步，尺寸随DPI缩放。完成后点击打开目录，确认内容后手动发送 ZIP；不自动上传。

“兼容性检查”页签把原 CLI 的默认命令搬进界面：填报告目录、最低指令集、最低内存，点“开始检查并生成报告”，结果与 `-batchmode` 完全一致，并写同一份 `OSGameEnvironmentReport.txt` / `.json`。“GPU 选卡验证”页签是原 `ZGpuSelectionInspector`：上半部分是离线回归用例，下半部分是本机真实探测，改 BIOS 开关独显后点“重新探测”即可复查。

只读采集CPU、内存、BIOS/整机/主板型号、SMBIOS内存配置、原始微码注册表字节、显卡/驱动、双显卡自动选卡验证（`gpu-selection.json`）、磁盘容量、事件诊断、WHEA及已有内存诊断结果，并在项目配置了日志目录时附带最新的游戏日志副本。不读取序列号、不运行内存测试、不重启、不改BIOS、不加载GameCore或扫描DLL。每个采集阶段在独立受Job管理的子进程执行，默认45秒超时，失败/取消保留已完成信息。系统权限不足、字段缺失、事件覆盖不完整会记录在报告中，不代表硬件健康。

### 游戏日志打包（gamelogs 阶段）

环境报告只有系统侧证据时，往往看不出游戏自己打印了什么，因此新增 `gamelogs` 阶段：把游戏日志目录里**窗口内最新的** `.log` / `.logcompr` / `.txt` 复制进报告目录并打进ZIP，索引写入 `game-logs.json`。`.logcompr` 是游戏在 `Cache\Logs` 下写的压缩会话日志，漏掉它就等于没有游戏自己的记录。

- 目录由 profile 声明，不猜测：`logRoot`（`localLow` 默认，另支持 `localAppData` / `roaming` / `programData`，必须是这几个已知目录之一）+ `logSubPath`（相对路径，禁止绝对路径、`..`、驱动器号）。两者都不会拼出日志目录之外的路径。OSGame 配置为 `logRoot=localLow` + `logSubPath=Tencent\osgameCNOfficial\Cache\Logs`（即 `%USERPROFILE%\AppData\LocalLow\Tencent\osgameCNOfficial\Cache\Logs`）。未配置 `logSubPath` 的 profile 不运行该阶段，manifest 也不会出现它。CLI 可用 `--log-dir <绝对路径>` 覆盖。
- 挑选顺序：先取事故时间 ±窗口内写过的日志，其余按最后写入时间倒序；上限 8 个文件、单文件 16MiB、合计 48MiB，超限文件不截断、只记 `skipped`（`TOO_LARGE` / `TOTAL_LIMIT` / `UNREADABLE`），避免把半个日志伪装成完整日志。
- 只复制不修改：以 `FILE_SHARE_READ|WRITE|DELETE` 打开，游戏仍在写也能读；不跟随目录/文件链接（拒绝重解析点）；副本落到本次报告目录，命名 `gamelog-NN-<原名>.log`（非白名单字符替换为 `_`），原文件不改名、不删除、不截断。
- 不递归到底：只扫描日志目录及其下一层子目录（深度上限3层由 `MaxGameLogScanDepth` 控制，当前只走一层）。
- 索引记录每个副本的原始相对路径、字节数、修改时间、SHA256 与 `complete`（复制期间被追加导致短读时为 false）；日志目录路径用 `SanitizePath` 去掉用户名。
- 状态语义：目录不存在或窗口内没有日志时返回 `NO_LOGS`（manifest 中体现），**不计入 `partial`**——这不是采集失败，也不代表机器有问题；`game-logs.json` 仍然写入并给出 `reason`（`NOT_CONFIGURED` / `LOG_DIRECTORY_MISSING` / `NO_LOG_FILES_IN_WINDOW` / `NO_LOG_FILES_COLLECTED`）。
- 隐私：日志可能含账号、聊天等信息。副本只留在本地报告目录，不自动上传；`summary.txt` 在包含日志副本时追加提示，发送前需人工确认。`EnvironmentCollectorTests` 覆盖：只收 `.log`/`.txt`、名字白名单化、副本真实落盘、缺目录与空目录返回 `NO_LOGS`、profile 路径解析。

微码仅保存 `Update Revision` 原始字节，不声称跨CPU统一解码或应用桌面Intel门槛。内存标称/配置速率不等于XMP判定。BIOS日期不是安装日期。没有WHEA或内存检测结果不等于通过检测。

### CLI与配置

```powershell
.\ZEnvironmentCollector.exe -batchmode --collect --profile .\profiles\osgame.ini --output-dir .\Reports --include-events
.\ZEnvironmentCollector.exe -batchmode --diagnose --diagnose-detail --incident-time 2026-10-12T18:30:00+08:00 --incident-kind crash --output-dir .\Diagnosis
.\ZEnvironmentCollector.exe -batchmode --diagnose --diagnose-from-events .\Diagnosis\OSGameDiagnosisEvents.tsv --output-dir .\Replay
.\ZEnvironmentCollector.exe -batchmode --gpu-selection
.\ZEnvironmentCollector.exe --profile .\profiles\osgame.ini          # GUI，指定profile
.\ZEnvironmentCollector.exe --tab gpu                                 # GUI，直接打开GPU页签
```

`--collect` 还支持 `--incident-time`、`--incident-kind crash|hang|unknown`、`--lookback-hours 1..720`、`--stage-timeout-seconds 1..300`、`--log-dir <游戏日志目录>`（覆盖profile配置，用于临时指向其它日志目录）；未填事故时间按已有事件查找，不伪装成当前时间。退出0表示ZIP生成，完整性见manifest；30为错误。

`--gpu-selection` 打印离线回归用例与本机真实探测：回归全部通过退0，任一用例选错退1（说明 `GfxDeviceSetup.cpp` 的选卡逻辑回退了），工具本身异常退30。本机不支持 `EnumAdapterByGpuPreference` 不算失败，只在文本里说明，仍退0。

`--collect-stage` 与 `--wer-config` 是内部入口：采集器会以受 Job 管理的子进程、或提权 `ShellExecute` 的方式重新启动自己（`ToolExecutablePath()`，即同目录的 `ZEnvironmentCollector.exe`）。它们即使不带 `-batchmode` 也不会弹窗，玩家不要直接调用。

GUI 默认读取同目录 `profiles/default.ini`；其他项目只需设置 `[project]` 下的 `id`、`displayName`、`gameExecutable`、`lookbackHours`，需要附带游戏日志时再加 `logRoot`（默认 `localLow`）与 `logSubPath`。配置支持ASCII或UTF-16LE BOM编码，项目ID与进程名仅允许安全文件名字符。无目标游戏时使用generic，不能把其它进程推断成目标游戏。

保留原 `--diagnose` 的JSON schema1、四类verdict、事件回放、退出0/30语义及原OSGame文件名；原普通CLI的GPU mock/实际GPU选择探针和DLL行为保留。普通CLI并不是安全采集入口：`--check-dll`会追加路径而不是关闭递归扫描。玩家用GUI或 `--collect`。

### 1.8.0：推荐Windows系统转储（WER）与已有dump导入

高级区域默认选择“Windows系统崩溃转储（推荐）”，另保留“ProcDump附加（需允许调试）”。WER不用选择或运行ProcDump：选择当前项目游戏exe → 点击开始 → 确认完整内存隐私及注册表修改 → 批准UAC → 界面提示系统捕获已启用后，正常启动游戏并复现崩溃。默认等1小时。目标需在当前用户会话运行；不同用户的游戏不会自动放宽输出目录权限。

**一次启用可连续收集多次崩溃**：启用后配置一直生效，每次崩溃各自复制一份（第一份为 `crash.dmp`，之后为 `crash-2.dmp`、`crash-3.dmp`…，单次会话上限8份），点击“安全停止捕获”、等待超时或达到上限后才统一恢复注册表配置。这是行为变更：此前收到第一份dump就立即恢复配置，导致同一会话内后续崩溃完全不被收集（玩家反馈“连续崩两次都没反应”就是这个原因）。工具版本号仍为1.8.0，未随本次变更调整。每份dump独立做完整内存/异常线程/PID/模块路径校验，晚到的dump不会被当作重复文件跳过。

只写64位注册表视图的 `HKLM/SOFTWARE/Microsoft/Windows/Windows Error Reporting/LocalDumps/<exe文件名>` 三值：DumpType DWORD=2、DumpCount DWORD=2、DumpFolder REG_EXPAND_SZ=本次私有目录。WER按文件名匹配而非完整路径，因此其他同名程序也可能生成文件；采集器必须结合观测到的PID、创建时间、dump异常时间和完整模块路径才能自动接收。仅做可选的有限查询，不申请VM_READ、不调试附加、不修改反作弊。进程保护导致身份证据不足时保留原文件，提示手工确认导入，不把任意新文件当目标崩溃。

普通环境采集仍只读。提权动作仅在用户启用/恢复WER时发生，调用随包CLI的严格 `--wer-config` 内部入口；参数不进入shell执行。备份写入 `HKLM/SOFTWARE/ZEngine/EnvironmentCollector/WerBackupsV1/<exe>`，ACL只允许管理员/SYSTEM修改，普通用户只读。先持久化原值的存在性、类型、原始字节与预期新值，再写目标值；异常退出后可用“恢复当前项目WER原配置”重试。恢复逐值比较，若其他工具已改动，返回冲突并保留备份，不覆盖它们。空备份/部分应用均有恢复路径；不直接删整个LocalDumps键。**恢复是恢复先前配置，不是关闭已有全局WER；以前FullDump.bat建立的全局设置不会被本工具删除。** UAC拒绝/恢复冲突时明确提示，窗口退出前对未恢复配置再次告警。

结束收集（用户停止/等待超时/达到份数上限）后按原配置恢复（可能再次弹UAC），再补采环境、计算SHA256和ZIP64压缩。多份dump全部打进同一个 `capture.zip`，`checksums.json` 逐份记录SHA256；`capture.json` 新增 `capturedDumpCount` 与 `dumps` 数组逐份记录PID、异常码、dump时间，顶层单份字段仍指向第一份以兼容旧读取器。用户主动停止且已收集到dump时按正常完成处理（不再视为取消、不再生成无dump的失败包）；一份都没收到时仍为取消/超时失败。停止标志只用于结束等待，不再传给后处理，否则会立刻自我取消。会话目录为 `EnvironmentReports/crash-capture-wer-<GUID>`，其 `system-dumps` 保留Windows原文件，验证后的副本为 `crash.dmp`，最终分享包为该私有目录内的 `capture.zip`，不把完整内存包放到可能共享的父目录。目录权限限当前用户、管理员和SYSTEM；不跟随目录链接，不覆盖已有文件。打开捕获结果按钮直接选中ZIP。空间不足/超时/后处理失败生成仅含capture.json的capture-failure.zip，不冒称含dump。停止不会终止游戏、WER或删除仍在写入的文件；晚到文件仍可手工导入。完整dump仍可能数GB，至少4GiB起步，复制前要求额外约2倍dump大小+1GiB；单dump上限64GiB。

“选择已有dump并打包”不改注册表、不运行ProcDump，不删除或移动原文件；需先选择对应游戏exe并确认来源和完整内存隐私。验证完整内存/异常线程/PID/模块路径后复制、补采与打包。报告记录 `captureBackend=dump_import` 和 `identity=USER_CONFIRMED_IMPORT_AND_MODULE_PATH`，不伪称属于本次运行。1.8.0共用打包流程也将ProcDump最终包放到本次目录内的 `capture.zip`。WER/import报告schemaVersion=2，记录配置/恢复状态、身份依据、dump时间与异常码；ProcDump原schema不变。保留1.7.3事件优先窗口、逻辑像素DPI和滚动条修复，GUI事件明细仍默认勾选。

验证（2026-10-14）：Release独立构建和4项CTest通过；HKCU唯一测试键覆盖原值不存在、原始类型/字节还原、已有备份拒绝覆盖、Token错配、外部修改冲突、Token写入前中断清理；合成dump的模块与进程创建时间校验通过。只对 `EnvironmentCaptureTestTarget.exe` 做真实HKLM启用后取消/恢复、系统异常落dump/身份匹配/恢复/打包，后者生成82,451,550字节完整dump；Python独立回读ZIP CRC、白名单和SHA256通过，最终ZIP ACL仍私有，目标配置与备份均确认移除。首次系统测试错误复用了含SEM_NOGPFAULTERRORBOX的ProcDump测试模式，导致无WER文件；仅为测试程序新增允许WER的wer-crash模式后重跑通过，没有修改系统策略。已有dump导入端到端通过。GUI 150%实际截图与鼠标选择验证两个后端，截图在独立build的wer-validation内。

连续收集端到端验证：同一会话内对 `EnvironmentCaptureTestTarget.exe` 触发两次真实崩溃，一次启用、一次UAC恢复，得到 `crash.dmp` 82,451,550字节与 `crash-2.dmp` 82,468,078字节，两份SHA256不同（确为两次独立崩溃而非重复复制），`capturedDumpCount=2`，收到第2份后停止即打包 `capture.zip`，`HasWerBackup` 确认配置已恢复。取消路径与已有dump导入路径重跑仍通过。

限制：真实WXQ.exe及玩家反作弊环境尚未验证；自定义崩溃处理、自动调试设置或系统策略可能阻止WER，配置成功不保证生成dump，不用于卡死/普通退出。未完成跨用户目标、标准用户UAC拒绝、异机ACL/受控文件夹环境验收；当前只支持安全exe文件名、本地普通目录，文件路径映射不同的历史dump需匹配原游戏路径。工具未数字签名、不自动上传。说明来源：https://learn.microsoft.com/en-us/windows/win32/wer/collecting-user-mode-dumps 。

### 高级诊断：一次完整崩溃dump（1.7.2历史行为，1.8.0变更以上节为准）

1.7.2修复“签名通过但访问游戏失败，没有verification.zip且打开普通报告目录”：1.7.1只在签名前置失败生成verification.zip，并且GUI普通采集和捕获共用结果对象。捕获失败后执行普通采集，会覆盖取证目录入口。现在普通报告与捕获结果分别保存；新按钮“打开捕获结果（定位ZIP或capture.json）”只使用本窗口最近一次捕获结果，直接在资源管理器选中ZIP或capture.json，不受普通采集影响。高级区域显示版本号。若重开窗口无历史结果，提示到桌面EnvironmentReports/crash-capture-*查找，不跳转普通报告冒充。

签名前置失败继续生成`verification.zip`；签名后的进程访问/监控/后处理异常生成`capture-failure.zip`，二者仅含capture.json，不混入dump或原始日志。收集根目录不可写、授权前参数失败、取消/正常退出等仍可能只有本地JSON或没有新目录，不能承诺任何失败都必有ZIP；磁盘打包失败明确提示发送JSON。报告补充failedStage、failedOperation、Win32错误码、目标PID和requestedProcessAccess；OpenProcess失败、进程已退出、路径变化、已有调试器分别提示。不自动提权、不调整真实目标DACL、不启用调试特权、不跳过签名或反作弊。旧截图的“目标进程已变化或访问被拒绝”只证明本次已通过签名且未附加；不能由截图确认错误码5或断言反作弊是原因。

1.7.2验证：3项CTest通过；手动集成测试`access-denied`对仅含合成数据的专用进程限制访问，产生真实`OpenProcess(capture)`错误5、OPENING_TARGET阶段，签名已通过而ProcDump未启动，生成capture-failure.zip，Python独立回读仅capture.json。首次测试运行继承了调试特权，DACL拒绝未生效并导致测试失败；已仅在测试进程内禁用该特权，并新增实际拒绝访问前置断言后重跑通过，生产采集器权限未改。GUI真实文件选择/授权触发未签名拒绝，再点击普通采集，再点捕获结果入口，资源管理器回读仍选中原crash-capture-*下verification.zip。验证截图和产物位于独立build的validation-1.7.2；未访问真实游戏。下面保留1.7.1历史说明，按钮名及异常包扩展以本节为准。

1.7.1补充签名前置失败诊断：旧1.7.0将吊销查询、证书链、摘要、缺失签名和发布者不符统一显示为一句错误，并且验证发生在取证目录建立前，失败没有capture.json。新版保留WinVerifyTrust直接返回的LONG及十六进制值、错误分类、签名者/颁发者、文件SHA256/版本、策略和`procDumpStarted`；前置失败自动保存仅含`capture.json`的`verification.zip`。点击“打开dump取证目录”发送此包，不要重复发送顶部普通`osgame-*.zip`。失败ZIP不是dump，也不表示捕获成功；未通过验证不会启动ProcDump，不会记住该文件。

2026-10-13反馈截图与`osgame-2026-10-13T054234144Z-16144-0.zip`只能确认玩家在验证阶段被阻止。该ZIP为1.7.0普通环境报告、仅8文件，无ProcDump哈希、签名状态或dump；无法据此断言断网、文件篡改或反作弊阻止。此版是定位版，不声称已修复玩家的实际证书环境。严格吊销/微软身份校验保持不变，不加离线放行、不导入证书、不修改系统时间或关闭安全软件。

验证：3项CTest通过；未签名测试文件被拒绝且返回失败诊断包；模拟错误分类覆盖0x80092013、0x80096010及非微软发布者；日志路径不可写不阻断JSON/ZIP；独立zipfile回读包内仅capture.json并确认started/verified=false。本机官方12.01签名只验证通过，未执行它；未在玩家机器复现真实错误。新版成功捕获链路本轮未重新附加测试进程，沿用1.7.0既有验证。

用户打开GUI并滚动到“高级诊断”，选择微软官方 `procdump64.exe` 和当前profile对应的游戏exe（不是Launcher）。点击开始后需分别确认完整内存隐私和Sysinternals许可；选择否不会启动监控。ProcDump需从 https://learn.microsoft.com/en-us/sysinternals/downloads/procdump 自行下载，采集器只提供打开官网入口，不捆绑或镜像分发。通过签名/原始文件名/64位PE检查才运行，并保持只读文件句柄防止校验后修改。证书在线验证不可用时拒绝运行，不降级跳过。

- 目标约束：名称匹配profile、完整路径匹配用户选择、同一Windows会话、单个64位实例；确认无已有调试器/ProcDump。以PID调用 `-accepteula -ma -e -n 1`，不使用全局AeDebug注册，不注入游戏、不主动启动游戏。默认等待及监控合计1小时，正常退出或未捕获异常时只保存失败/无dump清单，不假报成功；游戏重启后需再次点击开始。
- 空间：预检至少4GiB，且不低于预计已提交内存两倍加2GiB，监控中持续检查1GiB安全余量。完整dump最大64GiB；空间不足拒绝启动或请求安全停止。完整dump可能包含账号/聊天/令牌和完整路径，无法脱敏；仅本地保存并经安全渠道手动发送，ZIP本身未加密。
- 停止：只通过微软支持的 `ProcDump-<目标PID>` 事件请求优雅退出并等待恢复目标。该事件影响同目标的所有ProcDump实例，因此启动前拒绝已有监控。绝不把现有helper的KillOnJobClose/TerminateProcess用于ProcDump。普通关闭窗口等待安全退出；异常清理也走同一保护，若安全入口不可用会等待而不是强杀。不能保证用户强杀采集器、操作系统关机等场景仍可完成取证。
- 完成判定：等待ProcDump退出并关闭输出文件；联合完成文本、支持的退出码、MDMP头、FullMemory标志、Memory64负载范围、Exception/Thread/MiscInfo、目标PID验证，再标记dump有效。官方12.01实测重定向输出为UTF-16LE、成功一份dump返回1，因此不能按普通exit0规则判断。其他输出格式未知时保留原始文件并报告未确认，不猜成功。
- 结果包：`crash.dmp`、`environment.zip`、`capture.json`、`procdump.log`、`checksums.json`。环境子包沿用原白名单；清单保存同意时刻、目标名/PID、EXE文件版本和SHA256、ProcDump版本及SHA256、异常码、dump时间/大小、验证结果。EXE文件版本不保证等同游戏资源版本，定位仍需结合dump模块及发布记录获取匹配PDB。原始dump始终保留；后处理取消/失败不删除它。
- 日志/哈希：ProcDump日志保存最多1MiB输出并转UTF-8。成功后所有证据文件计算SHA256，按白名单流式Deflate压缩成ZIP64并实际解压回读CRC。64KiB文件读缓冲，不把数GB文件装入内存。大dump打包可取消；环境子包的短暂最终阶段维持原不可取消行为。
- 设置：采集器将成功完成签名检查后的ProcDump路径记在 `HKCU/Software/ZEngine/EnvironmentCollector/ProcDumpPath`；每次GUI仍需重新确认许可与内存采集，偏好写失败不丢取证结果。ProcDump可能自行保存其EULA接受状态。没有修改全局调试器注册和反作弊设置。
- 压缩依赖：`engine/3rdparty/minizip-ng`（主仓子模块，编辑器 XlsxImporter 用的同一份），以 deflate 单方法构建，ZIP64 由库原生支持，无补丁、无本地修改。整树构建时复用 `engine/3rdparty/CMakeLists.txt` 已建好的 `minizip-ng` 目标；独立构建时由本工程 `add_subdirectory` 并按需 FetchContent 拉取 zlib（本工程本来就 FetchContent zydis，联网配置是既有前提）。旧的 `tools/renderdoc/3rdparty/miniz` + `miniz-zip64.patch` 方案已移除：不再依赖 RenderDoc 子模块、不再在配置期跑 `git apply`、不再需要 Git。交付源码须包含 `LICENSE-minizip-ng.txt`（安装时从子模块 LICENSE 改名复制）。归档仍为流式：64KiB 读缓冲，不把数GB文件装入内存；写完回读每个条目完整内容并由库比对 CRC32。

验证在独立build的 `capture-validation`：3项CTest通过；官方ProcDump12.01配合仅含合成数据的 `EnvironmentCaptureTestTarget.exe` 验证完整异常dump自动打包、正常退出不误报、等待取消、已附加后取消（目标仍活着且无调试器）、进度回调抛异常后安全脱离；完整dump错误PID/截断尾部拒绝。4GiB+1KiB合成文件通过独立Python解压、大小、ZIP64版本45和CRC验证。GUI文件选择及拒绝隐私授权路径已用真实窗口消息验证并截图。未附加真实游戏，反作弊/CrashSight共存、权限提升、低磁盘实际故障仍需内部实机验证。当前微软页面列ProcDump支持Windows11及以上，此模式不承诺Windows10兼容。

### ZIP契约与隐私

从1.7.0起ZIP采用流式Deflate/ZIP64，极小文件可由库选择Store；UTF-8文件内容，压缩完成后回读CRC校验。普通环境报告仍最多32项、单文件16MiB、总内容64MiB；高级证据包单文件64GiB、总内容128GiB。只收明确生成的白名单文件：`basic.json`、`firmware.json`、`diagnosis.json`、`health-events.json`、`graphics.json`、`storage.json`、`summary.txt`、`manifest.json`；同意明细时加 `events-redacted.json`。JSON各有schemaVersion。manifest记录版本、项目、UTC时间、事故类型、同意项、各阶段状态及实际包内文件。

默认不附原始XML/全系统日志/用户目录/卷标签/磁盘序列号。健康事件只保留错误类型、APIC/MCA和检测结果等白名单字段；其它程序名脱敏。`events-redacted.json` 是分享用格式，不冒充可回放原始TSV；研发回放仍用明确开启 `--diagnose-detail` 的本地TSV。程序名脱敏成 `OTHER_PROCESS` 后，自 1.8.1 起由 `isTargetGame` 布尔位回答"是不是指定的游戏"，仍不暴露进程名。

窗口使用系统字体，不随包复制Windows字体。图形初始化失败会尝试WARP，再失败提示CLI采集。系统字体、D3DCompiler或权限异常可导致GUI无法启动，CLI仍可单独使用。

### 独立构建与分发

独立构建默认包含GUI；从引擎父项目嵌入时默认关闭GUI（`ENVIRONMENT_CHECK_BUILD_GUI=OFF`），因为整树里 ZSlate 已经是 `/MD`，与本工具的 `/MT` 链接会 LNK2038。**关闭GUI不会少一个exe**：同一个 `ZEnvironmentCollector.exe` 仍然会构建，只是换成 `EnvironmentCheckGuiFallback.cpp`，双击会提示改用 `-batchmode`，命令行能力完全不变。不同运行库配置须使用独立build目录。

```powershell
cmake -S engine\tools\WindowsEnvironmentCheck -B build\tools\ZEnvironmentCollector -G "Visual Studio 17 2022" -A x64 -DENVIRONMENT_CHECK_BUILD_GUI=ON -DBUILD_TESTING=ON
cmake --build build\tools\ZEnvironmentCollector --config Release --target ZEnvironmentCollector EnvironmentCollectorTests EnvironmentCollectorUiTests EnvironmentCaptureTests EnvironmentPreflightTests
ctest --test-dir build\tools\ZEnvironmentCollector -C Release --output-on-failure
cmake --install build\tools\ZEnvironmentCollector --config Release --component EnvironmentCollector --prefix dist
```

### 打包分发（zip + SHA256）

参考扁鹊（`E:\BianqueCCS\pack`）的做法：打包是一个**独立的、不编译任何东西的 CMake 工程**
`engine\tools\WindowsEnvironmentCheck\pack\`，只把已经 staging 好的安装树用 CPack 打成一个包。
之所以不放进工具自己的 `CMakeLists.txt`：那份文件也会被整树 `add_subdirectory()`，
CPack 会在一个从没打算发东西的构建里被配置一遍。

```powershell
cmake --install build\tools\ZEnvironmentCollector --config Release --component EnvironmentCollector --prefix dist\staging
cmake -S engine\tools\WindowsEnvironmentCheck\pack -B build\tools\ZEnvironmentCollector\pack
cmake --build build\tools\ZEnvironmentCollector\pack --target package
```

产出 `dist\<名字>-windows-x86_64-<版本>.zip` 与同名 `.zip.sha256`，名字区分两种构建：

- 自包含构建 → `zenvironment-collector-windows-x86_64-<版本>.zip`
- 引擎后端构建 → `zenvironment-collector-zruntime-windows-x86_64-<版本>.zip`

多出的 `zruntime` 由 pack 读 exe 导入表自动加上，不用手填——两种构建曾产出**完全同名**的两个包，
内容却不同，只能打开才知道是哪个。

- 版本号从 `ToolVersion.h` 解析（与报告里的 `toolVersion` 同一个来源，不可能对不上）；可用
  `-DPACK_VERSION_OVERRIDE=x.y.z` 覆盖。
- 架构从生成器探测，可用环境变量 `USER_DEF_ARCH` 覆盖。
- 包内是一个以包名命名的顶层目录，展开后 `README.md` + `WindowsEnvironmentCheck\`
  （exe、`ZDllLoadProbe.exe`、`profiles\`、LICENSE）——就是能直接跑的那个目录，没有多余的层。
- ZIP 用 CMake 自带的归档能力生成，不需要装 7-Zip。
- **两种构建打出的包同名但内容不同**：独立构建自包含（无 DLL）；引擎后端（全树构建）多出
  `ZRuntimeShared.dll` 与 `resource\ui_strings_zh-CN.zasset`。pack 会读 exe 的导入表判断是哪一种：
  引擎后端但 staging 里没有 DLL 时**直接配置失败**（缺 DLL 是 `0xc0000135`、进程根本起不到
  `main()`，而包看起来完全正常）；缺 zasset 只告警，因为 UI 会退回英文而不是死。
- staging 目录里没有 `ZEnvironmentCollector.exe` 时**直接配置失败**并打印该跑的那条命令：
  把一个空的/过期的 staging 目录打成一个看起来很像样的包，是这里唯一一种"看起来成功了"的失败。

产物不落在 build 目录里：`ZEnvironmentCollector.exe` 与 `ZDllLoadProbe.exe` 输出到
`bin\Tools\<Config>\`（子目录名与源码目录同名），profiles 与 LICENSE 也复制过去，所以该目录
就是可直接打包的分发单元——而不会在共享的 `bin\Tools\<Config>\` 里和另外三个工具的 exe 混在一起。

**构建目录按配置带后缀**，因为 `bin\Tools\<Config>\` 是全仓库每一棵构建树共用的：

| 配置 | 构建目录 |
|---|---|
| 独立构建 | `WindowsEnvironmentCheck\` |
| 引擎后端 `/MD` | `WindowsEnvironmentCheck-zruntime-md\` |
| 引擎后端 `/MT`（分发用） | `WindowsEnvironmentCheck-zruntime-mt\` |

两个维度都需要：只按后端分的话，两棵全树（`/MD` 与 `/MT`）仍然会互相覆盖同一个 exe，
而 `cmake --install` 只会忠实地把最后落地的那个装走——包里的 exe 就悄悄变成了另一个构建。
**安装目录不受影响**，始终是 `WindowsEnvironmentCheck\`（`--prefix dist` 装出
`dist\WindowsEnvironmentCheck\`），那是包内的契约目录，不随配置变。

分发 `ZEnvironmentCollector.exe`、`ZDllLoadProbe.exe`、profiles及LICENSE文件，不只发主 exe。无需ZRuntime或引擎资源；Release采用静态CRT。GUI和ZIP模式未改变旧环境报告schema5。GUI动态依赖Windows的 `d3d11.dll`、`D3DCOMPILER_47.dll` 及基础系统DLL。

1.6.1 将问题类型改为ZSlate下拉框，支持鼠标选择、方向键/回车、Esc取消、外点关闭且不穿透底层控件。启用PerMonitorV2，窗口/字体按物理DPI更新而不重建控件；修正字形对齐时额外拉伸1像素的错误，测量与绘制使用相同整数像素字号。

验证：Release构建成功，CTest核心与UI共2/2通过；UI覆盖100/125/150/200%下下拉选择/关闭/键盘/边界翻转、字形与UV的1:1映射（含裁剪）、指针重置保留编辑焦点及失焦清理。实机150%：旧版窗口DPI=96且awareness=0，新版DPI=144且awareness=2；本轮成功取得中文及下拉展开截图，并通过真实鼠标输入验证外点按住移动不穿透、Esc后第一次点击有效、选择卡住生成hang报告及ZIP校验。截图位于独立build的 `validation-1.6.1-final`。这些测试不是与ImGui/UE同字体同字号的视觉等价证明。

仍为内部试用：实际跨显示器DPI切换、WARP故障回退、各厂商BIOS/WHEA字段和受限权限需进一步实机验证。GUI使用Windows 10 1607及以上的DPI API。最后打包阶段不可取消。尚未数字签名，不作为已完成现网发布验收的产物。

## 独立构建（CLI only）

只想出命令行包时关掉GUI，产物仍是同一个 `ZEnvironmentCollector.exe`：

```powershell
cmake -S engine\tools\WindowsEnvironmentCheck `
  -B build\tools\ZEnvironmentCollectorCli `
  -G "Visual Studio 17 2022" `
  -A x64 `
  -DENVIRONMENT_CHECK_BUILD_GUI=OFF

cmake --build build\tools\ZEnvironmentCollectorCli `
  --config Release `
  --target ZEnvironmentCollector
```

构建使用固定版本及 SHA-256 的 Zydis 4.1.1 和匹配的 Zycore。首次配置需要网络下载依赖，之后可复用 CMake FetchContent 缓存。
