# 打包 → 独立 Player → 呈现帧 端到端闭环（2026-11-05）

## 结论

第七节「一条真实项目的端到端闭环」的第 3 步已经跑通，并且**是在一个没有源资产树的项目里跑通的**：

```
ZEditor --cvar "bundle.build standalone --force"
    -> <project>/Build/Standalone/*.zbundle + manifest
    -> 隔离项目（无 asset 内容、无 Intermediate/）启动 ZGame
    -> 16 bundle(s) mounted，0 个 ZPak 错误
    -> level object graph built (32 objects) / level load succeed (yaml)
    -> 渲染线程跑帧 -> exec.after N quit -> 退出码 0
```

**⚠️ 重要纠正：第一次「隔离项目加载成功」是假绿。** 负向对照（把隔离项目的所有 `.zbundle` 清空、`0 bundle(s) mounted`）显示关卡依然 `32 objects` 完整加载。原因见缺陷 #6：`AssetManager::ReadObjectsFromYaml()` 是裸 `fopen`，绕过 `SerializedFile::InitializeRead()` 的 bundle 钩子，而 `GetFullPath()` 又把相对路径解析回开发机的引擎源码树 —— 挂载 bundle 只是摆设，关卡一直从源仓库偷读。修掉 #6、并在验证脚本里加上「日志必须出现该关卡由 bundle 提供」的断言之后，闭环才真正成立。这也说明：**「mount 成功 + 关卡加载成功」不是证据，「拿掉 bundle 就失败」才是。**

在此之前所有打包验证都停在 C++ 单元级；玩家进程一直靠源树回退在跑，而日志里没有任何一条能区分「读包」和「读盘」。

修复后的最终验证：`python scripts/verify_packaged_player_e2e.py --frames 10` → **9/9 断言全绿**，其中 `level bytes served from a bundle` 和 `non-empty object graph` 是为杜绝假绿新增的决定性断言。

可重复执行形态：`python scripts/verify_packaged_player_e2e.py --frames 60`。
CI 形态：`.github/workflows/packaged_player_e2e.yml`（`workflow_dispatch` 手动触发，见下方「CI 状态」）。

## 环境事实

| 项 | 值 |
|---|---|
| 仓库 | `E:/Engine/ZEngine`，main，VS2022 / Debug / DX12 |
| 工程 | `engine/ZGameSmoke.zproject`，`startup_level: asset/level/1-1.scene` |
| 打包入口 | ZEditor 控制台 `bundle.build`（`--cvar` 在系统启动后执行） |
| 运行入口 | `bin/Debug/ZGame.exe`，新增 `--cvar` |
| 隔离项目 | `build/_packaged_e2e/PlayerProject_<时间戳>/` |

## 首跑暴露的六个真实缺陷（全部已修）

单元测试、编辑器内点菜单、以及"看日志有没有报错"都发现不了这些，因为源树回退会把它们全部掩盖。

1. **registry key 基址错**（`engine/Source/Editor/Build/AssetBundleBuilder.cpp`）
   `EditorAssetManager.cpp:148` 用 `m_WorkingDir`（项目根）作 `AssetRegistry` 扫描根，key 形如 `asset/level/1-1.scene`；builder 却用 `GetProjectContent()`（`engine/asset`）作基址，拼成 `engine/asset/asset/level/1-1.scene`。结果 31 个 root 里绝大多数 `cannot read asset ...; skipped`，首包只有 15 个 asset / 23,984 bytes。
   修：改用 `registry.GetScanRoot()`（缺失时回退 `GetProjectRoot()`），与 `EditorAssetManager::TryGetPathForGuid` 和运行时 `ResolveExternalAssetPath` 的解析保持一致。

2. **空 `shared` 包仍被声明为依赖**（同文件）
   依赖条件看的是 `!shared_assets.empty()`（依赖图"想"共享什么），而不是实际写进 bundle 的条目。条目全被跳过时 `shared.zbundle` 根本不存在，`MountBundle` 报 `needs missing bundle 'shared'`。
   修：改成检查 `bundles[kSharedBundleName]` 是否真有条目。

3. **`shared` ↔ `scripts` 依赖环**（同文件）
   `shared` 依赖 `scripts`、`scripts` 又依赖 `shared`，`MountBuild` 报 `dependency cycle` 并 **0 bundle(s) mounted**，而玩家照常从源树启动，看起来"一切正常"。
   修：两个都是基础设施包，只允许 root bundle 依赖它们，二者互不声明。

4. **注册表缓存里的历史绝对 key 被当 root**（同文件）
   水合的 `AssetRegistry` 缓存残留 `E:/.../*.scene` 形式的 key，打包会为每个资源再产一份名字被 mangle 的重复 bundle，还把 PIE 临时场景当发布内容。
   修：自动发现 root 时过滤绝对 key 与 `Saved/` 前缀；显式 `options.roots` 不受影响。

5. **构建产物目录被当成可发布 root，逐次指数累积**（同文件）
   registry 的扫描根是项目根，因此 `Intermediate/Cooked/<Platform>/` 里 cook 出来的 `.scene` 也会被索引成 `LevelRes` root。每次 `bundle.build` 生成的新 cooked 场景在下一次又变成新 root，bundle 名一层层叠 `Intermediate_Cooked_Standalone_`（实测叠到 4 层），文件名最终长到 `cannot open bundle`，挂载报 1 条 `[E] [ZPak]`、33 个 bundle 只挂上 32 个。这不是人畜无害的"产物冗余"，是会随打包次数恶化的真实故障。
   修：自动 root 发现时一并排除 `Intermediate/` 与 `Build/` 前缀；cooked 场景的**字节**仍作为依赖走正常依赖遍历进包，只是不再当 root。

6. **YAML 关卡读取完全绕过 bundle 钩子**（`engine/Source/Runtime/Resource/Asset/AssetManager.cpp`）
   `SerializedFile::InitializeRead()` 的注释声称 `Level::load` 也走它，但 `Level::load()` 对 `.scene` 调的是 `ReadObjectsFromYaml()`，里面是 `fopen(path)`；只有二进制 `.zasset` 走 `SerializedFile`。同时 `AssetManager::GetFullPath()` 优先把相对 URL 解析到 `ConfigManager::GetRootFolder()`（开发机上的引擎源码树），于是本机永远有源场景可读。结果：一个真正没有 `Assets/` 树的发布包**根本打不开 `.scene` 关卡**，而开发机上的所有打包冒烟测试全绿。
   修：`ReadObjectsFromYaml()` 在读盘前先问 `AssetBundleManager::TryReadAssetBytes(path, bytes)`，命中就用内存字节并打一条 `[ZPak] ReadObjectsFromYaml: '<path>' served from a mounted bundle`；未命中才 `fopen`。`AssetBundle::GetAssetBytes` 的后缀匹配让绝对路径也能命中相对 registry key。验证脚本把这条日志作为**决定性断言**，并要求 `level object graph built (N objects)` 且 N > 0（空关卡也会报 `level load succeed`）。

另外补齐一项无人值守能力：`ZGame` 此前只有 `--cvar-file`（仅 `name=value`），不能执行控制台命令，无法自己退出。现补 `--cvar`（`engine/Source/Game/ZGame.cpp` + `engine/Source/Player/PlayerShell.{h,cpp}`），与 ZEditor 的 `applyStartupConsoleVariables` 同语义：文件先、命令行后，`ExecuteString` 接受赋值也接受命令。`--cvar "exec.after 90 quit"` 让 player 跑满 N 帧后正常关停，退出码可信、日志完整 flush；不再需要外部 `taskkill`。

## 验证数据

| 阶段 | 修复前 | 修复后 |
|---|---|---|
| `bundle.build` 汇总 | 16 bundle / 15 asset / 23,984 B | 16 bundle / 42 asset / 29,376 B（中间态曾出现 21~33 bundle，均为 #5 未修时的自我增殖） |
| `asset/asset/` 跳过警告 | 大量 | 0 |
| `shared.zbundle` | 不存在 | 存在 |
| 依赖环 | 无（因为 shared 为空）→ 后出现 | 0 |
| `bundle(s) mounted` | 0 | 16 |
| `[E] [ZPak]` | 满屏 | 0 |
| 隔离项目关卡加载 | `32 objects` + `level load succeed`，但**从源仓库偷读** | `32 objects` + 日志明确 `served from a mounted bundle` |
| 无 bundle 负向对照 | `0 mounted` 却仍加载成功（假绿） | 关卡字节断言转红 |
| 无人值守退出 | timeout 强杀 | `Quit requested` + `All systems shut down`，EXIT=0 |

`scripts/verify_packaged_player_e2e.py` 的断言：bundle 挂载数 > 0、无 ZPak 错误、关卡加载成功、**该 startup level 的字节确实由 bundle 提供**、对象图非空、`OpenScene: loaded`、`exec.after` 已排队、干净关停、渲染路径有帧证据，以及**隔离项目里 startup level 文件确实不存在**。倒数第二/第三条是 #6 之后补的：没有它们，脚本会把"读源树"也判成通过。

### 已知的非打包侧错误（如实记录，未粉饰）

隔离运行的日志里仍有 `[E] [ZRender]`：

- `CreateComputePipelineState failed HRESULT=0x80004005` —— nanite GPU culling 降级为关闭，非致命。
- 退出时 DX12 debug layer 报 `This API cannot be called on a closed command list` 与一条 CBV 地址越界。
- `MainCameraRp1Pass: 30 render entities exist but none are visible` —— 视锥剔除/可见性的既有问题，渲染管线确实在跑帧，但没有可见实体。

这些都属于渲染侧既有欠账，与本次打包链修复无关；脚本只把 `[E] [ZPak]` 作为失败条件，并单独打印 `ZRender_errors` 计数。

### 工程坑（本机）

- bash 宿主下 `cmake --build` 静默返回 0、零输出；必须用 PowerShell 宿主，并在同一调用里删掉重复的 `PATH` / `path` 键（MSBuild 起 CL.exe 时会 `MSB6001`），加 `-- /p:BuildProjectReferences=false` 只编目标 TU。
- `PlayerShell.cpp` 属于 `ZPlayer` 库，改它之后**必须显式 `--target ZPlayer ZGame`**，否则 ZGame 链接期找不到新符号。
- 脚本第一次跑挂了 15 分钟：Windows GUI 进程派生的辅助进程继承 stdout 管道，`subprocess.PIPE` 永远等不到 EOF。改成文件重定向解决。
- 脚本不要批量 `rmtree` 隔离目录（工作区安全策略会拦）；改用时间戳新目录。
- **负向对照要真的破坏数据源**：只藏一个 root bundle 不够（关卡当时根本不从 bundle 读）；把全部 bundle 清空后仍然全绿，才把 #6 逼了出来。日志断言必须盯住「字节来自哪里」，而不是「加载成功了吗」。

## 第二轮：脱离源码树的安装 + 脚本 OnUpdate 真跑（同日续）

第七节五步闭环的第 3 步原本只做到「关卡来自包」。这一轮补上两件事：**把 ZGame.exe 装到与源码树完全隔离的目录**，以及**让打包后的 TypeScript 行为真的每帧执行**。可重复执行形态：`python scripts/verify_player_install_e2e.py --frames 30`。

做法：脚本自己生成一个最小工程（`.zproject` + 一个带 `TypeScriptComponent` 的 `probe.scene` + `.ts` / 编译 `.js` / `script_registry.json`，全部写在 `build/_player_install_e2e/`，**不碰仓库源资产**），用 ZEditor 打出 `asset_level_probe.zbundle` + `scripts.zbundle`，再把 `ZGame.exe` + 53 个 DLL + `ZEditor.ini` + 项目元数据 + `Build/Standalone` 复制到一个全新安装目录（`--out` 可指向仓库外，本机实测在 `C:/Users/zentiali/WorkBuddy/ZEngine/_player_install_probe/probe_20261105_081042/`）。因为 `ZEditor.ini` 的 `BinaryRootFolder=.`，引擎根就是该目录，`GetFullPath()` 再也无法回退到 `E:/Engine/ZEngine/engine/`。

安装目录里**故意没有** `asset/level/probe.scene`；`Intermediate/Scripts/ZPakProbe.js` 是一份会抛 `DISK_COPY_SHOULD_NOT_BE_USED` 的**诱饵**（只用于让 `ScriptRegistry` 保留条目，见缺陷 #9）。实测结果：

```
2 bundle(s) mounted, 0 个 [E]
ReadObjectsFromYaml: '.../asset/level/probe.scene' served from a mounted bundle (1158 bytes)
TypeScriptComponent bound: module='ZPakProbe' class='ZPakProbe'
[ZPakProbe] OnStart
[ZPakProbe] OnUpdate frame 1 / 2 / 3 / ... / 30 dt=0.0168
诱饵错误串出现 0 次  ->  编译模块来自 scripts.zbundle，不是磁盘
Quit requested + All systems shut down, EXIT=0
```

负向对照：拿掉 `scripts.zbundle` → `cannot open bundle`、`needs missing bundle 'scripts'`、关卡加载连锁失败、`MUTANT_EXIT=1`、**0 次 OnUpdate**；bundle 已还原。

脚本另以 `--out C:/Users/zentiali/WorkBuddy/ZEngine/_player_install_probe/script_run --frames 10` 在**仓库外**完整跑过一遍（生成工程、打包、stage 安装、断言全在 C 盘），同样 **13/13 全绿**；默认在 `repo/build/_player_install_e2e/` 跑也是 13/13。原有 `verify_packaged_player_e2e.py --frames 10` 在 #8 修复后回归 **9/9**。

### 这一轮逼出来的三个缺陷

| # | 缺陷 | 位置 | 状态 |
|---|---|---|---|
| 7 | **引擎全局配置未进包，且 `FileSystem::loadAssetJson` 同样绕过 bundle 钩子**：`asset/global/particle.global.json` 不在任何 `.zbundle` 里，`ParticleManager::Initialize()` 找不到就直接 `LOG_FATAL`；`1-1.scene` 里的 `*.material.json` / `*.skeleton.json` 也全没进包，运行日志一片 `open file ... failed`、`0 render entities`（这纠正了上一节把这些渲染错误归为「与打包链无关」的误判） | `engine/Source/Runtime/File/FileSystem.h`、`AssetBundleBuilder` 依赖遍历 | **已修（第三轮）**：`ReadAssetText()` 接入 bundle 钩子 + builder 4c 收文本资产 + `START_SYSTEM()` 前早挂载。详见下节 |
| 8 | **独立 Player 从不进入 play 态**：`g_isPlaying` 只有编辑器 Play 按钮和单元测试会置 true，`g_editorTickComponentTypes` 白名单也只由 ZEditor 填充；`GameObject::shouldComponentTick()` 因此在 Player 里对所有组件恒返回 false —— 渲染循环照常出帧，但**所有 Update / OnUpdate / 物理行为永远不执行** | `engine/Source/Runtime/Application/Application.cpp:46`、`BaseClasses/GameObject.cpp:24` | **已修**：`PlayerShell.cpp::OpenStartupLevel()` 开头 `g_isPlaying = true`（桌面 / 移动端 / Web 三条 Player 启动路径共用）。`g_isEditor` 保持 false，遵守 `Application.h` 约定 |
| 9 | **脚本 registry 依赖 TS 源码存在**：`ScriptRegistry::rescan()` 在 `Scripts/` 不存在时清空整张表，在缺源时把条目当 orphan 删除，并在缺 `Intermediate/Scripts/*.js` 时清空 `m_CompiledRelPath`；而发布包本不该带 `.ts` / `.js` | `engine/Source/Runtime/Scripting/ScriptRegistry.cpp:313` | **已修（第三轮）**：Player 模式跳过 `rescan()`、registry 从 bundle 水合，发布目录零 `.ts` / 零 `.js`。详见下节 |

### 发布布局约定（第二轮的临时形态，已被第三轮取代）

- 当时还必须带：`asset/global/`（#7）、`AssetRegistry/script_registry.json`、`Scripts/*.ts` 与 `Intermediate/Scripts/*.js` 元数据（#9）。
- 严格「零 `.ts` / 零 `.js`」版本当时实测**失败**（`no ScriptAsset for guid ...`），这就是 #9，不是脚本链本身的 bug。
- **现行约定见下节「第三轮 · 发布布局约定」**：上述脚手架已全部移除。

`scripts/verify_player_install_e2e.py` 的 13 条断言覆盖：两个 bundle 挂载、无 ZPak 错误、关卡字节由 bundle 提供、对象图非空、场景打开、`TypeScriptComponent bound`、`OnStart`、`OnUpdate` 多帧且跑到指定帧数、诱饵字符串 0 次、渲染路径出帧、`exec.after` 排队、干净关停。

## 第三轮：零源资产的真自包含（同日再续）

第二轮的 13/13 仍带三处从源码树搬来的东西：`asset/global/`（引擎全局配置 4 文件）、`.ts` 源副本、以及一份损坏的 `.js` 诱饵。它们都是 #7 / #9 的脚手架，不是交付形态。这一轮把两个缺陷修掉，安装目录收敛成**只有** `ZGame.exe` + 同目录 DLL + `ZEditor.ini` + `*.zproject` + `Build/Standalone/`；脚本断言从 13 条扩到 **18 条**。可重复执行形态：`python scripts/verify_player_install_e2e.py --frames 20`。

### 修复清单

| 项 | 位置 | 做法 |
|---|---|---|
| #7a 最后一个绕过 bundle 的读取器 | `Runtime/File/FileSystem.h` + `FileSystem.cpp` | 三个 `loadAssetJson` 重载、`LoadAssetByJson`、`LoadAssetByYaml` 原本全是裸 `fopen`。新增 `FileSystem::ReadAssetText()`（实现放在 .cpp，避免这个被广泛包含的模板头引入 `AssetBundleManager.h`），先问 `TryReadAssetBytes`，命中即用内存字节并打 `[ZPak] FileSystem: '...' served from a mounted bundle`，未命中才 fopen。编辑器不挂 bundle，行为不变 |
| #7b 文本资产没进包 | `Editor/Build/AssetBundleBuilder.cpp` 新增 4c 段 | 组件引用是 **路径字符串** 而非 GUID PPtr，`AssetRegistry::GetDependencies()` 永远看不到 `*.material.json` / `*.skeleton.json` / `*.mesh.json`；引擎全局配置更在 registry 扫描根之外。显式扫描项目根下所有 `.json`（排除 `Build/` `Intermediate/` `Saved/`）与 `ConfigManager::GetAssetFolder()/global/`，塞进 `shared.zbundle`。`AssetRegistry/script_registry.json` **特意保留**在扫描范围内，它是发布元数据 |
| #7 的隐藏时序问题 | `Player/PlayerShell.{h,cpp}`、`Game/ZGame.cpp` | **这是本轮最容易漏的一点**：`ParticleManager` / `RenderSystem` / `ScriptRegistry` 在 `START_SYSTEM()` 里读 JSON，而原来的 `MountPackagedContent()` 在其后；`MountBuild()` 又必须先读 `.zasset` manifest，依赖已初始化的 `AssetManager`，不能提前。新增 `PreparePackagedRuntime(working_dir)`：在 `START_SYSTEM()` **之前**用 `MountDirectory(bundles_dir)` 挂全部裸 `.zbundle`（不读 manifest），`MountBuild()` 随后补依赖元数据且幂等。**ZGame 的 WinMain / POSIX main 与 PlayerShell 的 `ZPlayer_Boot` 共三处**都必须调，漏一处就退回磁盘 |
| #9 registry 依赖 TS 源码 | `Runtime/Scripting/ScriptRegistry.{h,cpp}` | 新增 `SetPackagedPlayerMode()`，由 `PreparePackagedRuntime()` 在启动前设置；`rescan()` 在该模式下直接返回，保住 `LoadFromDisk()` 水合出的条目与 `m_CompiledRelPath`。`LoadFromDisk()` 去掉 `std::filesystem::exists()` 前置判断、改走 `ReadAssetText()`，于是发布目录里连 `AssetRegistry/` 都不需要。编辑器从不设置该标志，冷扫描 + 热重载行为不变 |
| 字节来源的可断言证据 | `Runtime/Scripting/Wrapper/ScriptEnv.cpp` | 原来只打 `module loaded: X (from <Intermediate/Scripts/X.js>)`，看起来像读了磁盘；补一条 `LoadModule('X'): N bytes served from a mounted bundle` |

### 验证结果

| 检查 | 结果 |
|---|---|
| 默认目录 `--frames 20` | **18/18 全绿**（`E:/Engine/ZEngine/build/_player_install_e2e/...`） |
| 仓库外 C 盘 `--out C:/Users/zentiali/WorkBuddy/ZEngine/_player_install_probe/strict_20261105_0850 --frames 10` | **18/18 全绿**，生成工程 / 打包 / stage / 断言全在 C 盘 |
| 原 `verify_packaged_player_e2e.py --frames 10` 回归 | **9/9**，17 bundle mounted / `ZPak_errors=0` |
| 安装目录内容 | 53 DLL + exe + ini + zproject + 3 个 `.zbundle` + manifest；**零 `.ts` / 零 `.js` / 零 `.scene` / 零 loose `.json` / 无 `asset/` 与 `AssetRegistry/` 里的文件** |
| 真实场景 `1-1.scene` 的 #7b 证据 | `FileSystem: ... served from a mounted bundle` 命中 **122 次**（material 69 / mesh 43 / skeleton 1 / global 2 等），`open file ... .json ... failed` **0 次** |
| 负向对照：藏 `shared.zbundle` | `MUTANT_EXIT=3765269347`（FATAL 崩溃）、`OnUpdate=0`、`FileSystem served=0`、`Cannot open registry file` ×1、`asset/global/particle.global.json load failed!` ×1；bundle 已还原 |
| 负向对照：藏 `scripts.zbundle` | `OnUpdate=0`、模块 bundle 日志 0 次、`cannot open bundle ... scripts.zbundle` + `cannot open '.../ZPakProbe.js'`；bundle 已还原 |

18 条断言里新增的决定性几条：早挂载发生在 `START_SYSTEM()` 之前、global 配置由 bundle 提供、registry JSON 由 bundle 提供、编译模块字节由 `scripts.zbundle` 提供、日志里没有 missing authored JSON、以及「磁盘零源资产却执行成功」。负向对照要先隔离 `Saved/Logs/*.log`（引擎会在同一天的日志文件里追加，直接读会混进上一轮成功记录，第一次做就因此读出了假的 `OnUpdate=20`）。

### 发布布局约定（第三轮起）

- **必须有**：`ZGame.exe` + 同目录全部 DLL、`ZEditor.ini`、`*.zproject`、`Build/Standalone/`（manifest + Bundles）。
- **禁止有**：任何游戏内容资产或脚本源（`asset/level/*.scene`、`asset/objects/**`、`Scripts/*.ts`、`Intermediate/Scripts/*.js`、`asset/global/*.json`、`AssetRegistry/*.json`）。关卡、引擎配置、组件 JSON、脚本 registry、编译模块**五类字节**都必须只来自挂载的 bundle。
- 引擎运行时会自己创建空的 `asset/` / `Saved/` 目录，那是解析 URL 的副产物，不是分发内容；验证脚本只禁止目录里出现文件。

### 如实记录的边界

- 最小 probe 场景本身**没有 mesh**，日志仍是 `0 render entities`（`scene has no CPU-side mesh geometry`）。本轮验的是「所有 JSON 与脚本字节来自包 + 脚本每帧执行」，**不是**「画面里有可见几何」；要做像素级画面验收还得给 fixture 加真实几何引用。
- 真实 `1-1.scene` 这一轮不再出现 `30 render entities exist but none are visible` 与 `open file ... failed`，但仍有 30 条 `[E] [ZRender]`（DX12 debug layer 的 `closed command list` 24 条、compute shader / CBV 若干），属渲染侧既有欠账；没有像素证据，不据此宣称可见性已修好。
- `.zbundle` 的 4c 段把项目根下所有 `.json` 都收进 `shared`，包括编辑器元数据；当前打包汇总从 42 asset 涨到 578 asset / ~5 MB（真实项目 `ZGameSmoke`），后续可按类型收窄。

## 第四轮：把脚本 stage 换成正式安装产物（同日续）

### 先纠正一个错误结论

第二轮 / 第三轮都写着「本机没有 `cmake --install` 的 ZGame 规则（全仓 `install()` 只在 3rdparty 里）」——**这是错的**。`engine/CMakeLists.txt:618` 一直有 `install(TARGETS ZGame RUNTIME DESTINATION .)`，`engine/Source/Runtime/CMakeLists.txt:2129` 还有一整套 SDK 安装规则。当时只 grep 了 `engine/Source/Game/CMakeLists.txt` 就下了全仓结论。

实测 `cmake --install build --config Debug --prefix <dir>` 的产物是**不可运行的**：

| 现象 | 实测 |
|---|---|
| 顶层 DLL | **0 个**：`ZGame.exe` 在 `<prefix>/`，`ZRuntimeShared.dll` 被 Runtime 的规则装到 `<prefix>/bin/`，53 个第三方 DLL 一个都没装 |
| 总文件数 | **1653**（含 Runtime 全部公共头、`lib/`、`share/`、`ZEnvironmentCollector/`） |
| 游戏内容 | 装了 `engine/asset/` 整套，含 **3 个 `.scene`** |
| 注释 | 仍写着「compiled JS under Intermediate/Scripts is still NOT packed into a bundle」，而 #9 已修 |

所以不是「没有规则」，而是**规则停留在 SDK 时代，且和第三轮「零源资产」的结论直接冲突**。

### 修法：独立 `ZGamePlayer` component

不动任何既有 SDK 规则（它们保持 Unspecified component，其它会话依赖的 SDK 安装用法不受影响），在 `engine/CMakeLists.txt` 新增一段：

- `install(TARGETS ZGame ... COMPONENT ZGamePlayer)` + `install(FILES ZEditor.ini ... COMPONENT ZGamePlayer)`，两者都落在安装根。
- **DLL 必须在 install 时枚举**：configure 时 `bin/<Config>/*.dll` 可能还不存在，所以不能用 `install(FILES)`；用 `install(CODE)` + `file(GLOB)` 从 `BINARY_ROOT_DIR/$<CONFIG>` 拷全部 53 个 DLL 到 `${CMAKE_INSTALL_PREFIX}`。生成的 `cmake_install.cmake` 里 `$<CONFIG>` 已展开成 Debug / Release / MinSizeRel / RelWithDebInfo 四个分支。
- `ZENGINE_PLAYER_PROJECT` 环境变量**在 install 时求值**（写成 `\$ENV{...}`），把 `<项目>/*.zproject` 与 `Build/Standalone/{manifest,Bundles}` 平铺进安装根。验证脚本的项目是运行时生成的，不该为它重新 configure。
- `verify_player_install_e2e.py` 默认改成真的调用这条命令（新增 `--stage-mode copy` 保留手工 stage 作为控制组），并新增两条断言检查 installer 的输出日志。安装规则一旦漂移，回归立刻转红。

用法：

```bash
# 只装 Player 运行时骨架（55 个文件）
cmake --install build --config Debug --component ZGamePlayer --prefix <dir>

# 完整自包含发布目录（61 个文件）
ZENGINE_PLAYER_PROJECT=<已 bundle.build 的项目> \
  cmake --install build --config Debug --component ZGamePlayer --prefix <dir>
```

### 验证

| 检查 | 结果 |
|---|---|
| `verify_player_install_e2e.py --frames 20`（默认走真实 install） | **20/20** |
| 控制组 `--stage-mode copy --frames 10` | **18/18**，两条 installer 断言被正确跳过 → 不是恒真 |
| 原 `verify_packaged_player_e2e.py --frames 10` 回归 | **9/9** |
| 骨架安装 | 55 文件：exe + ini + 53 DLL；零 `.scene` / `.h`，无 `bin/` 目录 |
| 完整安装 | 61 文件：骨架 + `ZProbe.zproject` + `Build/Standalone/`（manifest + 3 bundle）；`Build/Standalone/` 无嵌套 |
| 负向对照 A：不带 `ZENGINE_PLAYER_PROJECT` | 0 个 `.zproject`、0 个 manifest（53 DLL 仍在）→ 项目内容确实只由那段 install 规则提供 |
| 负向对照 B：在真实 install 目录里藏 `shared.zbundle` | `MUTANT_EXIT=3765269347`、`OnUpdate=0`、`particle.global.json load failed!`；已还原 |

### 第四轮续：CPack 也收敛到 Player 布局

`cmake --install --component ZGamePlayer` 修好后，CPack 是最后一条还会产出旧布局的路径。`build/CPackConfig.cmake` 的默认值实测是：

```cmake
set(CPACK_COMPONENTS_ALL "EnvironmentCollector;Unspecified;ZGamePlayer")
set(CPACK_INSTALL_CMAKE_PROJECTS "E:/Engine/ZEngine/build;ZEngine;ALL;/")
```

第三项为 `ALL` 时，安装阶段 `CMAKE_INSTALL_COMPONENT` 为空，`cmake_install.cmake` 里所有 `if(<component> STREQUAL ... OR NOT CMAKE_INSTALL_COMPONENT)` 分支都会执行——两套规则叠加。把两行都钉到 `ZGamePlayer`：

```cmake
set(CPACK_COMPONENTS_ALL "${ZGAME_PLAYER_COMPONENT}")
set(CPACK_INSTALL_CMAKE_PROJECTS
    "${CMAKE_BINARY_DIR};${CMAKE_PROJECT_NAME};${ZGAME_PLAYER_COMPONENT};/")
```

| 检查 | 结果 |
|---|---|
| `cpack -C Debug -G ZIP`（骨架） | 成功，ZIP **55 文件**：exe + ini + 53 DLL，0 `.scene` / 0 头文件 / 无子目录 |
| `ZENGINE_PLAYER_PROJECT=<已打包项目> cpack ...`（完整） | 成功，ZIP **61 文件**：骨架 + `ZProbe.zproject` + `Build/Standalone/`（manifest + 3 bundle） |
| 解压完整 ZIP 到全新目录后直接启动 `ZGame.exe` | `EXIT=0`、`OnUpdate` 10 帧、`served from a mounted bundle` 5 次、0 ZPak 错误 / 0 FATAL、干净关停 |
| 用解压出的 exe 跑 `verify_packaged_player_e2e.py --player <unpacked>/ZGame.exe` | **9/9**，17 bundle mounted / 0 个 ZPak 错误 |
| 重新配置后 `verify_player_install_e2e.py --frames 10` 回归 | **20/20** |
| **负向对照**：把两行改回 `ALL` / 全 component | staging **1707 文件**（3 `.scene`、1109 头文件、`bin/` 里 1 个 DLL），且**压缩阶段直接失败**：`share/doc/BqLog/docs/文章1_为何BqLog如此快 - 高性能实时压缩日志格式.MD` 无法转 CP437。旧布局不只是不可运行，连包都打不出来 |

环境坑：重新 configure 时 `FetchContent` 的 zycore subbuild 又撞上 MSB6001，这次是 `HTTPS_PROXY` / `https_proxy`（以及 `HTTP_PROXY` / `http_proxy`）大小写重复键；Bash 下 `env -u http_proxy -u https_proxy cmake -S . -B build` 可解。此前记录的是 `Path` / `PATH` / `path` 变体。

### 如实记录的边界

- **CPack 已收敛**（见上一节）：`cmake --install --component ZGamePlayer` 与 `cpack` 现在产出同一份布局。需要 SDK 包时把 `engine/CMakeLists.txt` 里那两行 CPack 设置注释掉即可恢复旧行为——但旧布局连 ZIP 都打不出来，见负向对照。
- `Build/Standalone/AssetBundleBuildCache.json` 是 builder 缓存，随 `Build/` 一起被安装；它在 `Build/` 下不违反零源资产约定，但严格发布可排除。
- 默认 `cmake --install`（不带 `--component`）仍是旧 SDK 布局，这是刻意保留的兼容行为；不要把它当 Player 发布产物。

## CI 状态（不是已完成项）

`.github/workflows/packaged_player_e2e.yml` 已提供：构建 ZEditor → 追加构建 `ZRuntime ZPlayer ZGame`（`build_windows.bat` 默认只编 ZEditor；第三轮改到了 `FileSystem.cpp` / `ScriptRegistry.cpp` / `ScriptEnv.cpp`，显式带上 ZRuntime 更稳）→ 跑 `verify_packaged_player_e2e.py`（9 条）→ 跑 `verify_player_install_e2e.py`（第四轮起默认走 `cmake --install --component ZGamePlayer` 的真实安装产物 + 脚本 OnUpdate，**20 条**）。`build_windows.bat` 会执行 `zbuild.py configure`，preset 的 `binaryDir` 是 `${sourceDir}/build`，所以 CI 上 `build/cmake_install.cmake` 存在；脚本也提供 `--build-dir` 可覆盖。

**默认只 `workflow_dispatch` 手动触发，没有挂到 push**：GitHub-hosted Windows runner 没有可用 GPU，DX12 能否落 WARP 未验证；在确认之前挂上去只会制造与代码无关的红灯。文件里保留了注释掉的 `push` / `pull_request` 段，确认 runner 具备 DX12 后取消注释即成为真正的回归门禁。**本地已实测全绿，CI runner 上未验证。**

2026-11-05 09:55 **已把本地提交推到默认分支**。当时远端 `main` = `45f0e619`；此后只追加了文档一致性提交，当前远端 HEAD 以 `git ls-remote origin refs/heads/main` 为准。推送前发现远端有 1 个独有提交 `8f0fc3e9`（android host-clang 工具链），与本地 3 个提交从 `de264933` 分叉；核对它与本会话改动、以及工作树 45 个并发会话在途文件均**零重叠**后，用普通 `git merge --no-ff` 合入，未 force、未 rebase、未改写已有提交，工作树完好。`git ls-tree origin/main` 已确认 `packaged_player_e2e.yml` 在默认分支上。

但**真实 dispatch 仍未执行**：本机没有 `gh` CLI、`GITHUB_TOKEN` / `GH_TOKEN` / `GITHUB_PAT` 全未设置、`~/.config/gh/hosts.yml` 不存在；匿名 `GET /repos/zentia/ZEngine/actions/workflows` 返回 0 个 workflow，匿名 `POST .../packaged_player_e2e.yml/dispatches` 返回 `HTTP 403`——认证后才可见 / 可触发。要跑一次需在 Actions 页面手动 dispatch，或提供具备 `actions:write` 的 token。在此之前 runner 的 DX12 / WARP 结论一律不算已验证，push / PR 门不放开。

### 首次真实 runner 运行（2026-11-05 10:09）

用户在 Actions 页面触发后拿到第一段失败日志：`./build_windows.bat debug` 在 configure 阶段报
`CMake Error at CMakeLists.txt:3 (project): Generator Visual Studio 17 2022 could not find any instance of Visual Studio.`
日志里同时有 `[INFO] Found Visual Studio` —— 这是**假绿**：`zbuild.py::check_requirements()` 当时只检查
`C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe` 文件存在，不验证 VS2022 实例或 MSVC 工具链。

根因：`windows-latest` 在 2026 年已迁移到 Windows Server 2025 / **Visual Studio 2026** 镜像
（`windows-2025-vs2026`），而 `presets/windows.json` 的 `windows_visual_studio` 预设固定
`generator: "Visual Studio 17 2022"`，CMake 4.4.5 因此找不到任何 17.x 实例。`windows-2022` 镜像仍预装
VS2022 Enterprise 17.14 + MSVC C++ 工作负载。

修复（本轮）：
- `packaged_player_e2e.yml` 与 `build_windows.yml` 的 `runs-on` 由 `windows-latest` 改为 `windows-2022`；
- 主链新增 `Verify Visual Studio 2022 toolchain` 步骤，用 vswhere 打印全部实例并在缺 17.x 时 fail fast；
- `zbuild.py` 的 VS 检查改为查询 `[17.0,18.0)` + `VC.Tools.x86.x64`，打印真实版本，查不到给明确警告
  （仍是警告，不阻断 Ninja / 其它 preset）。本机实测输出
  `Found Visual Studio 2022 with MSVC C++ tools: Visual Studio Professional 2022`，`check_requirements()` 返回 `True`；
- 两个 workflow 的 YAML 已用 PyYAML 真解析验证（`windows-2022` / 6 steps / 诊断步骤 13 行 pwsh）。

**runner 的 DX12 / WARP 仍未验证**：这次连编译都没走到。修复需推送后再 dispatch 一次才能判断，
push / PR 门继续关着。另外 `smoke_tests.yml`、`smoke_tests_windows_vulkan.yml`、`agentkit*.yml` 仍用
`windows-latest`，但它们不固定 VS generator，是否受影响需各自实跑确认。

## 剩余边界

- **缺陷 #7 / #9 已修**（第三轮）。剩余的是下面这些，不是发布布局本身的缺口。
- 带 TS 组件的最小场景**仍没有可见几何**（`0 render entities`，场景本身不含 mesh），验证的是「字节全部来自包 + 脚本每帧执行 + 渲染管线出帧」，不是「画面里有东西」。#7 修好后真实 `1-1.scene` 的 122 次 JSON 读取已全部来自 bundle，但要做像素级画面验收还得给 fixture 加真实几何引用。
- **ZSlate 的按钮渲染与鼠标操作**仍未上机（P26 只验证逻辑入口，见 `doc/PrefabSystem_Design.md` §8）。
- `AssetRegistry` 本身仍会索引 `Intermediate/` 与 `Build/`（扫描根是项目根）；builder 已过滤，但更彻底的做法是收紧扫描根，避免缓存里持续堆积无效条目。
- macOS 不构建 ZGame（`AGENTS.md` §2.24 既有缺口）；本闭环目前只在 Windows / DX12 验证。
- `.zbundle` 的 cook 目前仍只有贴图真正做了平台 cook，其它资产按原字节打包。
- **exe 脱离源码树已验证**（见「第二轮」）。「全仓没有 ZGame 的 `cmake --install` 规则」是第二 / 三轮的**误判**（当时只 grep 了 `engine/Source/Game/CMakeLists.txt`）；`engine/CMakeLists.txt:618` 一直有规则，只是产出错误布局。第四轮已用 `ZGamePlayer` component 收敛成正式安装产物，脚本默认走真实 install，CPack 也已收敛到同一布局。
- **上一节把 `30 render entities exist but none are visible` 归为「与打包链无关」是误判**：在真正隔离的安装目录里，材质 / 骨骼 JSON 全部 `open file failed`，根因是 #7，不是单纯的视锥剔除问题。
