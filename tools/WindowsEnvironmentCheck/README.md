# Windows Environment Check

`ZWindowsEnvironmentCheck` 是独立的 Windows x64 环境兼容性检查工具，用于在不启动 Unity 客户端的情况下收集硬件信息、检查 DLL 构建配置，并通过隔离子进程验证 `GameCore.dll` 能否加载。

## 产物

Release 构建后需要一起分发：

```text
ZWindowsEnvironmentCheck.exe
ZDllLoadProbe.exe
LICENSE-Zydis.txt
LICENSE-Zycore.txt
```

- `ZWindowsEnvironmentCheck.exe`：主检查程序。
- `ZDllLoadProbe.exe`：无 C/C++ CRT、无 AVX/AVX2 的隔离 DLL 加载探针。
- `LICENSE-*.txt`：静态链接的 Zydis/Zycore MIT 许可证。

不要单独分发主程序，否则动态探针会报告 `DLL_PROBE_INFRASTRUCTURE_FAILED`。

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
.\ZWindowsEnvironmentCheck.exe --output-dir .\EnvironmentReport
```

默认支持无 AVX 的旧 CPU。如果产品明确要求某些指令集，可以配置：

```powershell
.\ZWindowsEnvironmentCheck.exe `
  --require avx,avx2,fma,f16c `
  --min-memory-gb 8 `
  --output-dir .\EnvironmentReport
```

只检查指定 DLL：

```powershell
.\ZWindowsEnvironmentCheck.exe `
  --check-dll .\GameCore.dll `
  --output-dir .\EnvironmentReport
```

显式执行隔离加载：

```powershell
.\ZWindowsEnvironmentCheck.exe `
  --require none `
  --probe-dll .\GameCore.dll `
  --output-dir .\EnvironmentReport
```

## Intel SDE 模拟旧 CPU

Intel SDE 不随本工具分发，需要从 Intel 官方网站下载并接受其许可协议：

<https://www.intel.com/content/www/us/en/download/684897/intel-software-development-emulator.html>

模拟 Nehalem（例如 Intel Core i5-760）：

```powershell
.\ZWindowsEnvironmentCheck.exe `
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
  "toolVersion": "1.7.0"
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

## 事件诊断与玩家一键采集（1.7.0）

GUI 使用 ZSlate，不依赖 ZRuntime、编辑器或游戏进程。公共源码只在本目录维护；OSGame 通过 profile 和发布适配接入，G侧已删除迁移完成的旧源码，仅保留分发适配和接口说明。

### 玩家操作

解压完整工具包，运行 `ZEnvironmentCollector.exe`，选择问题类型、可选填写带时区的发生时间，点击“开始采集并生成 ZIP”。默认输出到桌面 `EnvironmentReports`。可选择附带脱敏事件明细，默认不选。完成后点击打开目录，确认内容后手动发送 ZIP；不自动上传。

只读采集CPU、内存、BIOS/整机/主板型号、SMBIOS内存配置、原始微码注册表字节、显卡/驱动、磁盘容量、事件诊断、WHEA及已有内存诊断结果。不读取序列号、不运行内存测试、不重启、不改BIOS、不加载GameCore或扫描DLL。每个采集阶段在独立受Job管理的子进程执行，默认45秒超时，失败/取消保留已完成信息。系统权限不足、字段缺失、事件覆盖不完整会记录在报告中，不代表硬件健康。

微码仅保存 `Update Revision` 原始字节，不声称跨CPU统一解码或应用桌面Intel门槛。内存标称/配置速率不等于XMP判定。BIOS日期不是安装日期。没有WHEA或内存检测结果不等于通过检测。

### CLI与配置

```powershell
.\ZWindowsEnvironmentCheck.exe --collect --profile .\profiles\osgame.ini --output-dir .\Reports --include-events
.\ZEnvironmentCollector.exe --profile .\profiles\osgame.ini
.\ZWindowsEnvironmentCheck.exe --diagnose --diagnose-detail --incident-time 2026-10-12T18:30:00+08:00 --incident-kind crash --output-dir .\Diagnosis
.\ZWindowsEnvironmentCheck.exe --diagnose --diagnose-from-events .\Diagnosis\OSGameDiagnosisEvents.tsv --output-dir .\Replay
```

`--collect` 还支持 `--incident-time`、`--incident-kind crash|hang|unknown`、`--lookback-hours 1..720`、`--stage-timeout-seconds 1..300`；未填事故时间按已有事件查找，不伪装成当前时间。退出0表示ZIP生成，完整性见manifest；30为错误。`--collect-stage` 为隔离worker内部入口，不用于玩家直接调用。

GUI 默认读取同目录 `profiles/default.ini`；其他项目只需设置 `[project]` 下的 `id`、`displayName`、`gameExecutable`、`lookbackHours`。配置支持ASCII或UTF-16LE BOM编码，项目ID与进程名仅允许安全文件名字符。无目标游戏时使用generic，不能把其它进程推断成目标游戏。

保留原 `--diagnose` 的JSON schema1、四类verdict、事件回放、退出0/30语义及原OSGame文件名；原普通CLI的GPU mock/实际GPU选择探针和DLL行为保留。普通CLI并不是安全采集入口：`--check-dll`会追加路径而不是关闭递归扫描。玩家用GUI或 `--collect`。

### 高级诊断：一次完整崩溃dump（1.7.0）

用户打开GUI并滚动到“高级诊断”，选择微软官方 `procdump64.exe` 和当前profile对应的游戏exe（不是Launcher）。点击开始后需分别确认完整内存隐私和Sysinternals许可；选择否不会启动监控。ProcDump需从 https://learn.microsoft.com/en-us/sysinternals/downloads/procdump 自行下载，采集器只提供打开官网入口，不捆绑或镜像分发。通过签名/原始文件名/64位PE检查才运行，并保持只读文件句柄防止校验后修改。证书在线验证不可用时拒绝运行，不降级跳过。

- 目标约束：名称匹配profile、完整路径匹配用户选择、同一Windows会话、单个64位实例；确认无已有调试器/ProcDump。以PID调用 `-accepteula -ma -e -n 1`，不使用全局AeDebug注册，不注入游戏、不主动启动游戏。默认等待及监控合计1小时，正常退出或未捕获异常时只保存失败/无dump清单，不假报成功；游戏重启后需再次点击开始。
- 空间：预检至少4GiB，且不低于预计已提交内存两倍加2GiB，监控中持续检查1GiB安全余量。完整dump最大64GiB；空间不足拒绝启动或请求安全停止。完整dump可能包含账号/聊天/令牌和完整路径，无法脱敏；仅本地保存并经安全渠道手动发送，ZIP本身未加密。
- 停止：只通过微软支持的 `ProcDump-<目标PID>` 事件请求优雅退出并等待恢复目标。该事件影响同目标的所有ProcDump实例，因此启动前拒绝已有监控。绝不把现有helper的KillOnJobClose/TerminateProcess用于ProcDump。普通关闭窗口等待安全退出；异常清理也走同一保护，若安全入口不可用会等待而不是强杀。不能保证用户强杀采集器、操作系统关机等场景仍可完成取证。
- 完成判定：等待ProcDump退出并关闭输出文件；联合完成文本、支持的退出码、MDMP头、FullMemory标志、Memory64负载范围、Exception/Thread/MiscInfo、目标PID验证，再标记dump有效。官方12.01实测重定向输出为UTF-16LE、成功一份dump返回1，因此不能按普通exit0规则判断。其他输出格式未知时保留原始文件并报告未确认，不猜成功。
- 结果包：`crash.dmp`、`environment.zip`、`capture.json`、`procdump.log`、`checksums.json`。环境子包沿用原白名单；清单保存同意时刻、目标名/PID、EXE文件版本和SHA256、ProcDump版本及SHA256、异常码、dump时间/大小、验证结果。EXE文件版本不保证等同游戏资源版本，定位仍需结合dump模块及发布记录获取匹配PDB。原始dump始终保留；后处理取消/失败不删除它。
- 日志/哈希：ProcDump日志保存最多1MiB输出并转UTF-8。成功后所有证据文件计算SHA256，按白名单流式Deflate压缩成ZIP64并实际解压回读CRC。64KiB文件读缓冲，不把数GB文件装入内存。大dump打包可取消；环境子包的短暂最终阶段维持原不可取消行为。
- 设置：采集器将成功完成签名检查后的ProcDump路径记在 `HKCU/Software/ZEngine/EnvironmentCollector/ProcDumpPath`；每次GUI仍需重新确认许可与内存采集，偏好写失败不丢取证结果。ProcDump可能自行保存其EULA接受状态。没有修改全局调试器注册和反作弊设置。
- 压缩依赖：读取主仓固定的 `tools/renderdoc/renderdoc/3rdparty/miniz` 原始源码，不构建RenderDoc、不修改子模块。两处ZIP64版本字段修复由Git差异生成并保存在本目录 `miniz-zip64.patch`；CMake复制原始文件到构建目录 `miniz-patched`，通过Git检查、应用及反向检查后仅编译该生成副本。构建需要Git；`ENVIRONMENT_CHECK_MINIZ_DIR` 必须指向未修改的固定版本源码，补丁不兼容会立即终止配置。Git仓库发现范围限制在构建目录，避免嵌套仓库静默跳过补丁。已验证生成副本逐字节等于此前修复源，RenderDoc工作区恢复干净。同时使用回写本地头API，完整CRC校验不跳过。交付源码须包含补丁与 `LICENSE-miniz.txt`，无需向RenderDoc上游推送。

验证在独立build的 `capture-validation`：3项CTest通过；官方ProcDump12.01配合仅含合成数据的 `EnvironmentCaptureTestTarget.exe` 验证完整异常dump自动打包、正常退出不误报、等待取消、已附加后取消（目标仍活着且无调试器）、进度回调抛异常后安全脱离；完整dump错误PID/截断尾部拒绝。4GiB+1KiB合成文件通过独立Python解压、大小、ZIP64版本45和CRC验证。GUI文件选择及拒绝隐私授权路径已用真实窗口消息验证并截图。未附加真实游戏，反作弊/CrashSight共存、权限提升、低磁盘实际故障仍需内部实机验证。当前微软页面列ProcDump支持Windows11及以上，此模式不承诺Windows10兼容。

### ZIP契约与隐私

从1.7.0起ZIP采用流式Deflate/ZIP64，极小文件可由库选择Store；UTF-8文件内容，压缩完成后回读CRC校验。普通环境报告仍最多32项、单文件16MiB、总内容64MiB；高级证据包单文件64GiB、总内容128GiB。只收明确生成的白名单文件：`basic.json`、`firmware.json`、`diagnosis.json`、`health-events.json`、`graphics.json`、`storage.json`、`summary.txt`、`manifest.json`；同意明细时加 `events-redacted.json`。JSON各有schemaVersion。manifest记录版本、项目、UTC时间、事故类型、同意项、各阶段状态及实际包内文件。

默认不附原始XML/全系统日志/用户目录/卷标签/磁盘序列号。健康事件只保留错误类型、APIC/MCA和检测结果等白名单字段；其它程序名脱敏。`events-redacted.json` 是分享用格式，不冒充可回放原始TSV；研发回放仍用明确开启 `--diagnose-detail` 的本地TSV。

窗口使用系统字体，不随包复制Windows字体。图形初始化失败会尝试WARP，再失败提示CLI采集。系统字体、D3DCompiler或权限异常可导致GUI无法启动，CLI仍可单独使用。

### 独立构建与分发

独立构建默认包含GUI；从引擎父项目嵌入时默认关闭采集GUI，避免重复定义ZSlate。不同运行库配置须使用独立build目录。

```powershell
cmake -S tools\WindowsEnvironmentCheck -B build\WindowsEnvironmentCollector -G "Visual Studio 17 2022" -A x64 -DENVIRONMENT_CHECK_BUILD_GUI=ON -DBUILD_TESTING=ON
cmake --build build\WindowsEnvironmentCollector --config Release --target ZEnvironmentCollector EnvironmentCollectorTests EnvironmentCollectorUiTests EnvironmentCaptureTests
ctest --test-dir build\WindowsEnvironmentCollector -C Release --output-on-failure
cmake --install build\WindowsEnvironmentCollector --config Release --component EnvironmentCollector --prefix dist\WindowsEnvironmentCollector
```

分发GUI、CLI、`ZDllLoadProbe.exe`、profiles及LICENSE文件，不只发GUI exe。无需ZRuntime或引擎资源；Release采用静态CRT。新增GUI和ZIP模式未改变旧环境报告schema5。GUI动态依赖Windows的 `d3d11.dll`、`D3DCOMPILER_47.dll` 及基础系统DLL。

1.6.1 将问题类型改为ZSlate下拉框，支持鼠标选择、方向键/回车、Esc取消、外点关闭且不穿透底层控件。启用PerMonitorV2，窗口/字体按物理DPI更新而不重建控件；修正字形对齐时额外拉伸1像素的错误，测量与绘制使用相同整数像素字号。

验证：Release构建成功，CTest核心与UI共2/2通过；UI覆盖100/125/150/200%下下拉选择/关闭/键盘/边界翻转、字形与UV的1:1映射（含裁剪）、指针重置保留编辑焦点及失焦清理。实机150%：旧版窗口DPI=96且awareness=0，新版DPI=144且awareness=2；本轮成功取得中文及下拉展开截图，并通过真实鼠标输入验证外点按住移动不穿透、Esc后第一次点击有效、选择卡住生成hang报告及ZIP校验。截图位于独立build的 `validation-1.6.1-final`。这些测试不是与ImGui/UE同字体同字号的视觉等价证明。

仍为内部试用：实际跨显示器DPI切换、WARP故障回退、各厂商BIOS/WHEA字段和受限权限需进一步实机验证。GUI使用Windows 10 1607及以上的DPI API。最后打包阶段不可取消。尚未数字签名，不作为已完成现网发布验收的产物。

## 原CLI独立构建

独立配置和构建：

```powershell
cmake -S tools\WindowsEnvironmentCheck `
  -B build\WindowsEnvironmentCheck `
  -G "Visual Studio 17 2022" `
  -A x64

cmake --build build\WindowsEnvironmentCheck `
  --config Release `
  --target ZWindowsEnvironmentCheck
```

构建使用固定版本及 SHA-256 的 Zydis 4.1.1 和匹配的 Zycore。首次配置需要网络下载依赖，之后可复用 CMake FetchContent 缓存。
