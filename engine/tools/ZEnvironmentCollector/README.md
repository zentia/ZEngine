# Windows Environment Check

`ZEnvironmentCollector` 是独立的 Windows x64 环境检查与采集工具：不启动游戏客户端，收集硬件与系统信息、检查 DLL 构建配置，并在隔离子进程里验证 `GameCore.dll` 能否加载。一个 exe，图形界面和命令行都在里面——调用方看下面，玩家看文末。实现与构建说明见 `BUILD.md`。

## Launcher 用法

launcher 只在游戏拉起前跑这一条：

```powershell
ZEnvironmentCollector.exe -batchmode --preflight [--game-dir <游戏目录>] [--game-exe <exe名>] [--min-spec <min-spec.json路径>] [--budget-seconds N] [--output-dir <目录>] [--quiet]
```

`-batchmode` 沿用 Unity/UE 的写法：不开窗口，结论写进输出目录、进度与错误写 stdout，退出码交给调用方。**一次只允许一个实例**，第二个直接退出（`31`）。默认最多 3 秒（`--budget-seconds` 1–30），到点按 `Inconclusive` 结束。只读取机器状态：不扫盘、不加载游戏 DLL、不写注册表、不提权。

> 其它入口（同样带 `-batchmode` 的 `--collect` / `--diagnose` / `--gpu-selection`）是给支持人员和 CI 用的，参数见 `--help`；launcher 不需要它们。

### 退出码

| 退出码 | 含义 |
|---:|---|
| `0` | 结论已写入 `OSGamePreflight.json` |
| `30` | 工具内部错误 |
| `31` | 已有实例在运行，本次未执行 |

### 读结论

`OSGamePreflight.json` 里 launcher 只需四个键：`verdict` / `confidence` / `userMessage` / `actions`。诊断报告共用同一套键，`"mode":"preflight"` 用来区分。

| verdict | 含义 | launcher 应当 |
|---|---|---|
| `Pass` | 没发现值得说的 | 不提示 |
| `Advise` | 可提示的环境风险（驱动版本不一致、选到集显、页面文件过小、缺 VC++ 运行库、空间不足、等重启、装在机械硬盘、路径含非 ASCII…） | 非模态提示，同一个提示只说一次 |
| `BelowMinimum` | 缺 AVX2、内存低于最低要求、非 64 位 Windows、没有显卡能初始化 D3D11、功能级别低于 `minimumFeatureLevel` | 明确提示，但不阻断启动 |
| `Inconclusive` | 没跑完（超时或探针失败） | 当作"没检查过" |

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
