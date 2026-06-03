# Auto-Import & File Watcher 设计路线图

> 范围：从 OS 文件系统到 ZEngine 编辑器的所有自动化资产流入路径 ——
> 包括跨进程拖拽（Windows 资源管理器 / Finder → ImGui 窗口）、内部
> 文件系统监听、外部源文件改动 → 自动 reimport。
>
> 本文档是 PR-PW1 / PR-PW2 之后的下一阶段路线图。落地时按 §5 的 PR
> 拆分逐个推进；每一阶段都对照 UE 的等价实现（参考 §4），不照搬
> 代码。Unity 等价实现穿插对比。

---

## 1. 当前实现盘点（Before this work）

### 1.1 已就绪的基础设施

| 组件 | 位置 | 跨平台后端 |
|---|---|---|
| `FileSystemWatcher` | `engine/Source/Editor/file_system_watcher/` | Win `ReadDirectoryChangesW` / Linux `inotify` / macOS `FSEvents` |

- 接口一致：`watchDirectory(dir)` + `setOnFileCreated/Changed/Deleted` +
  `setExtensionFilter({ ... })` + 主线程 `update()` 排空回调队列。
- 事件源在工作线程产生，通过 `std::queue + std::mutex` 渡到主线程；
  所有用户回调最终都在 `update()` 调用者的线程触发（即编辑器主线程）。

### 1.2 已接入的消费者

| 消费者 | watch 根 | 扩展过滤 | 行为 |
|---|---|---|---|
| `EditorAssetManager::m_file_watcher` | `<Project>/Assets/`（即 `getProjectContent()`） | `.zasset`（默认） | `refreshAsset` / `removeAsset` → AssetRegistry 增量索引 |
| `EditorAssetManager::m_data_watcher` | `<Project>/Data/` | `.csv`、`.xlsx` | `DataTableImporter::compileOne` / `XlsxImporter::compileOne` → 写出 `Assets/_Generated/Data/*.zasset` → 由 `m_file_watcher` 二次拾取入 AssetRegistry |
| `TypeScriptCompiler::m_js_watcher` | `<Project>/Intermediate/Scripts/` | `.js` | 触发 `ScriptingEngine::ReloadModule()`（或 P3 的纯日志路径） |
| `EditorApplication::Tick` | —— | —— | 每帧调 `EditorAssetManager::TickWatcher()`，按"file_watcher 先、data_watcher 后"顺序 drain |

### 1.3 未接入的项

| 项 | 状态 | 影响 |
|---|---|---|
| Shaders/ watcher（外部改 `.hlsl` → DX12 重新编译 + 通知活动 material） | ✅ 已接（PR-AI2，2026-05） | `m_shaders_watcher` + `DX12ShaderCompiler::invalidateCacheForSource` 200ms 去抖；下一帧 PSO 重建时按 mtime 落到新 DXIL |
| OS 跨进程拖入（资源管理器拖 `.png` 进 Project window） | ✅ 已接（PR-AI1，2026-05） | drop target 路由到 `AssetsMenu::convertAsset` |
| AutoReimport（外部改 `.png` / `.fbx` 源文件 → 自动 reimport 到 `.zasset`） | ✅ 已接（PR-AI3，2026-05） | 焦点驱动 + `<Project>/AssetRegistry/source_registry.json` sidecar，每帧最多 reimport 10 个 |
| Scripts/ 源文件 watcher → 增量 tsc | 🟡 部分（只 watch `Intermediate/Scripts/*.js`） | 整个项目走 tsc `--watch` 子进程，源 `.ts` 改动通过 tsc → `.js` → watcher 间接生效，已经够用 |

ImGui 内部拖拽（`BeginDragDropSource` / `BeginDragDropTarget` 配合
`EditorDragDrop::kPayload*`）已连：Hierarchy ↔ Project 之间的 Prefab 互
拖、scene 层级调整等。这些是**进程内** ImGui payload，与本文档讨论
的**跨进程 OS 拖拽**是两回事。

---

## 2. 设计目标

按优先级：

1. **G1 — 拖入即导入**：用户从 Windows 资源管理器把 `.png` / `.fbx` /
   `.wav` 拖进 Project window 的某个文件夹时，自动调用
   `AssetsMenu::convertAsset(src, target_dir)` 完成导入。落点 = 鼠标
   悬停的那个文件夹（与 UE Content Browser 一致），且严格遵守
   PR-PW2 的 `Assets/` 子树约束。
2. **G2 — Shaders/ 实时重编译**：外部改 `.hlsl` 后，DX12 后端立刻
   重新编译并把活动 material 推上 GPU 重绑（与现有
   `DX12ShaderCompiler` 的 include-mtime 失效逻辑天然契合）。
3. **G3 — AutoReimport**：外部改源文件 `.png` / `.fbx` / `.wav` 后，
   编辑器在 N 秒静默期后自动重新 Import，覆盖原 `.zasset`，并刷新
   inspector / 引用方。
4. **NG1 — 不引入新模块，不重写 watcher**：所有阶段只是给现有
   `FileSystemWatcher` 加 consumer，不动跨平台后端。

非目标：

- ❌ 不实现 UE 那种"文件锁定提示" / 防误删保护（编辑器层面靠 VCS）。
- ❌ 不实现 UE 的 `FFileCache` hash 比对（文件 mtime 已经够用 ——
  ZEngine 的 importer 都是确定性的，重复运行幂等）。
- ❌ 不做监视外部"目录"层面的 rename（Windows `ReadDirectoryChangesW`
  对目录 rename 报告不稳定；Unity 也只在下一次 Project 窗口刷新时
  才发现）。

---

## 3. 当前实现 → 目标的差距矩阵

| 维度 | 现在 | 目标 | 差距 |
|---|---|---|---|
| 跨进程拖入 | ✅ PR-AI1 | ImGui drop target → `convertAsset` | — |
| Shaders/ 监听 | ✅ PR-AI2 | `m_shaders_watcher` → DXIL cache invalidate | — |
| 源文件 reimport | ✅ PR-AI3 | 焦点驱动 + `source_registry.json` + `reimportAsset()` | — |
| ImGui 内部拖拽 | ✅（Hierarchy↔Project） | 不变 | —— |

---

## 4. UE / Unity 对照

完整的源码定位见 §6 附录。摘要：

### 4.1 UE 路径

- **跨进程拖入**：OS → `FSlateApplication::OnDragEnterFiles`（`Slate/
  SlateApplication.cpp:7277`）→ `FExternalDragOperation::NewFiles` →
  `SAssetViewItem::OnDrop` → `DragDropHandler::HandleDragDropOnItem` →
  `UContentBrowserAssetDataSource::HandleDragDropOnItem`（`Plugins/
  Editor/ContentBrowser/.../ContentBrowserAssetDataSource.cpp:3188`）
  → `UImportSubsystem::ImportNextTick(Files, FolderPayload->GetInternalPath())`
  → `IAssetTools::ImportAssets(Files, DestinationPath, ...)`。落点是
  鼠标悬停的虚拟文件夹，**不是固定 `/Game/`**。
- **文件监听**：`Engine/Source/Developer/DirectoryWatcher/` 模块，
  `IDirectoryWatcher::RegisterDirectoryChangedCallback_Handle(dir,
  delegate, handle, flags)`。AssetRegistry 在
  `AssetRegistry.cpp:1283` 调用 `OnDirectoryChanged` 做增量刷新。
- **AutoReimport**：`UAutoReimportManager`（`UnrealEd/Private/
  AutoReimport/AutoReimportManager.cpp`）+ `FContentDirectoryMonitor`
  状态机（`ProcessAdditions / ProcessModifications / ProcessDeletions
  / PromptUser`）。最终调
  `FReimportManager::Instance()->Reimport(Asset, ...)`。受
  `EditorLoadingSavingSettings.bMonitorContentDirectories` 开关控制。

### 4.2 Unity 路径

- **跨进程拖入**：Project window 直接接收 `EditorWindow.OnDragPerform`，
  `DragAndDrop.paths` 取拖入的文件路径，`AssetDatabase.MoveAssetsToTrash`
  / `AssetDatabase.CreateAsset` 入库。落点也是鼠标悬停的文件夹。
- **文件监听**：`AssetDatabase` 后台扫描 + `AssetPostprocessor` 钩子；
  没有原生的 OS 监听器（macOS 上有 FSEvents，但 Windows 上是定时
  扫描）。改外部文件后回 Unity 焦点时统一 refresh。
- **AutoReimport**：`AssetPostprocessor.OnPreprocess*` 在每次 import
  发生时回调；不是事件驱动的"自动"，而是"被动"（焦点切回来才发现）。

### 4.3 ZEngine 取舍

ZEngine 取 UE 的事件驱动路径（已经有跨平台 `FileSystemWatcher`），
但 importer 注册表层面更接近 Unity 的 `AssetPostprocessor` —— 我们已
有的 `AssetImporter` 接口（按扩展名注册）天然适合做 G3 的 importer
反查。

---

## 5. 分阶段路线图

### 5.1 PR-AI1：拖入即导入（Drop-target import）— ✅ Landed

**状态**：已实现。代码在
`engine/Source/Editor/editor_window/project_window/project_window.{h,cpp}`，
约 110 行新代码（含注释）。

**目标**：用户从 Windows 资源管理器把 `.png` 拖进 Project window 的
"Assets/Characters/Hero" 文件夹，自动产生
`Assets/Characters/Hero/<basename>.zasset`。

**前置**：PR-PW2 已就绪 —— `AssetsMenu::convertAsset(src, target_dir)`
的两段式落点逻辑（`target_dir` 在 `Assets/` 内则用，否则 fallback
到 `<Project>/Assets/`）正是我们需要的入库 API。

**实现要点**（与原计划的差异已在代码注释中详细说明）：

1. **OS → 编辑器通道**：复用 `WindowSystem` 已有的 `glfwSetDropCallback`
   + `registerOnDropFunc` 监听器机制，**不引入新模块**、**不污染**
   ImGui 内部 payload 命名空间。`ProjectWindow` 构造时 `registerOnDropFunc`
   一个 lambda；该 lambda 把 GLFW 的 `paths`（仅在回调期间有效）
   立刻 `std::vector<std::string>` 复制后入队。
2. **延后到 onGUI 末尾执行**：drop 实际处理 `executePendingOsDropImports()`
   接在 `executePendingDelete` / `executePendingPrefabCreate` 之后，
   沿用同一套 deferral 模式 —— `convertAsset` 触发的
   `EditorAssetManager` 写盘 + `buildEngineFileTree` 重建会让正在
   walk 的 `EditorFileNode*` 失效，必须等 ImGui 树遍历完才能动手。
3. **落点决议**：直接采用 `resolveDropTargetFolder(m_selected_node)`，
   与右键菜单 "Import…" 完全同义。**没有**做"鼠标 hover 命中检测"
   的 future-style 实现 —— GLFW 只把 drop 暴露为终点事件，没有
   `OnDragOver` 流，等到 drain 帧时鼠标可能已经离开候选 item。
   `m_selected_node` 是 80% 用户路径（先点选后拖入）能工作的最稳方案。
   PR-PW2 的 fallback 兜底确保即便 selected 在 Scripts/ 下，产物也会
   落在 `<Project>/Assets/`。
4. **扩展名守门**：drain 时调
   `AssetImportManager::findImporter(src_path)`，nullptr 即跳过并
   `LOG_WARNING(ZAsset, ...)`。等价于 UE 的 `IsImportExtensionAllowed`。
   **没做**鼠标光标 NotAllowed 反馈：GLFW drop 流没有 enter/leave
   阶段，没法在拖动过程中改光标；只能在 drop 后给日志。
5. **目录拖入**：明确跳过（与 UE `ContentBrowserAssetDataSource` 一致），
   日志一行 INFO。Bulk-import / 递归是 future work。
6. **多文件**：drain 时循环 `convertAsset`，每个文件独立 try。
   import 完成后 `m_last_file_tree_update = {}` 强制下一帧
   `buildEngineFileTree` 立即重建（默认是每秒一次）。

**测试要点**（手动）：

- 从资源管理器拖 `Foo.png` 进 Project window 的某个 `Assets/` 子
  文件夹 → 该文件夹下出现 `Foo.zasset`，AssetRegistry 立刻索引（下一帧 1Hz tree rebuild 触发）。
- 拖 `Bar.exe`（无 importer）→ 日志一行
  `Drop-import: no importer registered for '...'`，不产生 `.zasset`。
- 拖到 `Scripts/` 根 → fallback 到 `<Project>/Assets/Foo.zasset`
  （遵循 PR-PW2 fallback 规则），同时日志告知最终落点。
- 一次拖多个文件（如 3 个 .png）→ 全部导入，日志汇总
  `Drop-import: 3 imported, 0 skipped (target: ...)`。
- 拖一个文件夹 → 日志 INFO 跳过，无副作用。

**需要改的文件**（估算 ~150 行）：

- `engine/Source/Editor/EditorApplication/EditorApplication.cpp`
  + `engine/Source/Editor/editor_window/editor_window_system.{h,cpp}`：
  注册 GLFW drop 回调，把路径塞进 `OsDropQueue`（编辑器全局单例或
  附在 `EditorWindowSystem`）。
- `engine/Source/Editor/editor_window/project_window/project_window.cpp`：
  - 每帧顶部 drain `OsDropQueue`，与当前鼠标 hover 的 Project window
    节点合流，决定落点目录。
  - hover 阶段调 `AssetImportManager::canImport(ext)` 设置
    `ImGui::SetMouseCursor(ImGuiMouseCursor_NotAllowed)` 反馈。
- `AGENTS.md` §2.10 末尾追加 1 段 "Drop-import" 规则（落点继承
  PR-PW2）。

**对照 UE**：等价于 `UContentBrowserAssetDataSource::
HandleDragDropOnItem` + `UImportSubsystem::ImportNextTick`，但
ZEngine 直接同步调用 `convertAsset` 即可（导入耗时短，没必要
defer 到下一帧；如果将来 importer 变重，可以再引入异步队列）。

**验收标准**：

- 从资源管理器拖 `Foo.png` 进 Project window 的某个 `Assets/` 子
  文件夹 → 该文件夹下出现 `Foo.zasset`，AssetRegistry 立刻索引。
- 拖 `Bar.exe`（无 importer）→ 鼠标光标变 NotAllowed，drop 后
  日志一行 warning，不产生 `.zasset`。
- 拖到 `Scripts/` 根 → fallback 到 `<Project>/Assets/Foo.zasset`
  （遵循 PR-PW2 fallback 规则），同时日志告知用户落点。

---

### 5.2 PR-AI2：Shaders/ 实时重编译 — ✅ Landed

**状态**：已实现。代码分布：

- `engine/Source/Runtime/Function/Render/interface/dx12/
  dx12_shader_compiler.{h,cpp}`：新增静态方法
  `DX12ShaderCompiler::invalidateCacheForSource(path)`，约 60 行。
- `engine/Source/Editor/EditorAsset/EditorAssetManager.{h,cpp}`：新增
  `m_shaders_watcher` + `m_pending_shader_invalidations` +
  `m_shader_debounce_mutex` + `queueShaderInvalidation` /
  `flushPendingShaderInvalidations`，约 130 行。

**目标**：外部改 `<Project>/Shaders/Foo.hlsl` → DX12 后端立即
重新编译产物 → 通知所有引用 Foo 的 material / pipeline 重绑。

**实现要点**（与原计划差异详见代码注释）：

1. **新增 `m_shaders_watcher`**：归在 `EditorAssetManager`，与
   `m_data_watcher` 同址。watch root = `ProjectInfo::getShadersRoot()`。
2. **扩展过滤**：`{".hlsl", ".hlsli", ".shader", ".compute", ".raytrace"}`。
   `.hlsli` 事件在缓存层目前是 no-op（cache key 是 top-level src，
   不是 header），但 `dx12_shader_compiler.cpp` 已有的
   `scanIncludesRecursive` mtime 兜底会让下次依赖编译自然 miss
   缓存 → 编辑 header 仍能触发依赖 .hlsl 重编译，只是延迟到用户
   触发的下一次 PSO 构建那一刻。未来如要做 header→referencer 反向
   索引以实时刷新，是单点扩展，不动 cache key。
3. **缓存失效 API**：选择**静态方法 + 默认目录**而非 watch 端拼路径，
   原因：`buildCacheFilePath` 用的 `fnv1a64` / `toLower` / `toHex16`
   helpers 故意放在 `dx12_shader_compiler.cpp` 的匿名命名空间里
   （不污染 header）；如果 watcher 端自己拼前缀，会出现两份必须同步
   的派生逻辑，cache key 一旦变就静默漂移。集中到 `DX12ShaderCompiler`
   静态方法是单一真相源。
4. **触发链**：
   - `onShaderEvent(path)` → `queueShaderInvalidation(path)` 上锁
     upsert 200 ms deadline 到 `m_pending_shader_invalidations`。
   - `TickWatcher()` 每帧调 `m_shaders_watcher.update()` →
     `flushPendingShaderInvalidations()`：先在锁内把已到期项搬到
     局部 vector，然后**释放锁**后逐项调
     `DX12ShaderCompiler::invalidateCacheForSource(src)`，匹配
     `<src_hash>_*.dxil` 前缀，一次性删掉所有 (stage, entry,
     defines, target_profile, hlsl_version) variant。
   - 下一次 PSO 构建/preview 渲染对该 shader 的 `compileFromFile` /
     `compileFromSource(shader_name=real_path)` 调用即 cache miss →
     DXC 重新编译。无需广播事件。
5. **PSO 重建**：故意不做主动广播。当前 ZEngine 体量下，PSO 是
   lazy 重建（每次 use 前 check）。如果未来引入持久 PSO 池，再加
   `MaterialManager::onShaderInvalidated(src)` 反查接口；仍走
   "dirty PSO + lazy rebuild" 而不是 UE 那套
   `FShaderCompilingManager` 全局事件总线。
6. **Inspector hot-reload**：`shader_preview_renderer` 走的是
   `compileFromSource` + `shader_name=preview_source.path`，是
   real on-disk path → 自动 hit 我们的 cache → cache 被 invalidate
   后下次预览自动重编。零额外代码。
7. **防抖**：200 ms。VS Code save 的 "write temp + rename" 序列
   会在 ~30ms 内打出 1×Created + 1×Changed；200 ms 把它们合到
   一次 invalidate。Unity 用 250 ms，UE 用 500 ms，我们取最短。
8. **Shutdown**：`m_shaders_watcher.stopWatching()` + 一次
   `flushPendingShaderInvalidations()`（deadline 未到的项不动）。
   未到期项跨会话由 cache 自身 mtime check 兜底（`last_write_time
   (cache) < last_write_time(src)` → miss → 重编），无需强制 flush。

**测试要点**（手动）：

- 用 VS Code 改 `<Project>/Shaders/<Name>.hlsl` 一行常量保存 →
  日志 200 ms 内出现
  `DX12 shader cache: invalidated N variant(s) for source '...'`
  + `PR-AI2: shader watcher invalidated N cache entries across 1
  source file(s)`。下次该 shader 被引用时自动 DXC 重编。
- 一次 "Save All" 改两个 .hlsl → 200 ms 内一行汇总日志，N 是
  两文件 variant 总和。
- 改 `.hlsli` → 当帧 `invalidateCacheForSource` 返回 0（hash 没匹配
  到 cache 文件），无副作用；下次依赖该 .hlsli 的 .hlsl 真正
  编译时由 `scanIncludesRecursive` 的 include-mtime check 判定
  cache 过期，再触发实际重编。
- 删除一个 .hlsl → 同样走 invalidate 流程，cache slot 删除（如果
  之前编过的话），无 ZEngine 崩溃。

**对照 UE / Unity**：

- UE：`UEditorEngine::ShaderRecompileRequested` +
  `FShaderCompilingManager` 的事件总线 + per-shader job 队列。
  ZEngine 当前体量用不上，单 watcher + invalidateCacheForSource
  + lazy recompile 已足够。
- Unity：`AssetDatabase.ImportAsset` 的同步导入 + ShaderImporter
  在 main thread 卡住几秒。我们的方案延迟更低（仅 200 ms 防抖 +
  下次 use 时编译），但卡顿点漂到首次渲染该 shader 的那一帧 ——
  对编辑器场景可接受，对 in-game shader hot-reload 不适用（如要
  支持运行时热重载，就得加 PR-AI2.5：异步 DXC + PSO 替换）。

---

### 5.3 PR-AI3：AutoReimport（外部源文件改动）✅ Landed

**实施状态**：已合入 main（2026-05）。下面分两部分：先列**最终落地的设计**，
再列**与原始 5.3 计划的差异**。

#### 5.3.A 最终落地设计

**触发模型**：焦点驱动（Unity 风，非 UE 实时事件）。

- `WindowSystem` 注册 GLFW `glfwSetWindowFocusCallback`，并暴露
  `registerOnWindowFocusFunc(std::function<void(int focused)>)` 让
  其他系统订阅。
- `EditorAssetManager::Initialize` 注册一个回调 `[this](int focused){
  if (focused != 0) onEditorFocusGained(); }`。
- `onEditorFocusGained()` 走 `SourceAssetRegistry::forEach`，对每条
  `(zasset, source_abs_path, source_mtime_ns)` stat 一次源文件，把
  mtime 变化的 zasset 入队 `m_pending_reimports`。
- 入队后置 `m_reimport_paused_until_focus = true`，避免在 ImGui
  窗口间点击造成的焦点抖动（macOS 上很常见）反复扫描。
- `EditorAssetManager::TickWatcher` 末尾调 `tickReimportQueue()`，
  最多处理 `kReimportsPerFrame = 10` 个：查注册表 → 找 importer →
  调 `importer->reimport(zasset, source, settings)` → 刷新
  `AssetRegistry` → 更新 `source_mtime_ns`。
- 队列空时清掉 `m_reimport_paused_until_focus` flag，等下次焦点。

**持久化**：sidecar JSON，**不**改 `.zasset` 头。

- 新增 `engine/Source/Editor/EditorAsset/SourceAssetRegistry.{h,cpp}`：
  - 落地到 `<Project>/AssetRegistry/source_registry.json`（与
    `script_registry.json` 同目录，**入版本控制**）。
  - JSON shape：`{ "version": 1, "entries":
    [ { "zasset", "source", "mtime_ns" }, ... ] }`。
  - 原子写：tmp + rename，Windows 上 fall back 到 `copy_file +
    remove`。
  - 路径规范化：`_WIN32` 上 lowercase（与 ScriptRegistry 一致），
    避免大小写不同的克隆机器命中错位。
  - 是 **composition** 成员（不是 `IEngineSystem`），由
    `EditorAssetManager` 持有 `SourceAssetRegistry m_source_registry;`。
- 写入入口：`AssetsMenu::convertAsset` 在 `importAsset` 成功后调
  `editor_asset_mgr->recordImportSource(output_zasset, source_abs)`。
  通过 `std::dynamic_pointer_cast<EditorAssetManager>(GET_SYSTEM(
  AssetManager))` 拿到具体类型（`IEngineSystem` 是 polymorphic）。
- 删除入口：`EditorAssetManager::onFileDeleted` 调
  `m_source_registry.removeEntry(path)`。

**Importer 接口扩展**：

`AssetImporter` 加新虚函数（**带默认实现**，存量 importer 不需要改）：
```cpp
virtual bool reimport(const std::filesystem::path& zasset_path,
                      const std::filesystem::path& source_path,
                      const AssetImporterSettings& import_settings)
{
    AssetMetadata md;
    return import(source_path, zasset_path, import_settings, md);
}
```

**TickWatcher 顺序**（刻意，勿调）：`m_file_watcher → m_data_watcher
→ m_shaders_watcher → tickReimportQueue()`。reimport 写出的
`.zasset` 在**下一帧**被 `m_file_watcher` 拾起做 AssetRegistry
增量刷新——这一帧延迟与 data-watcher 的设计理由相同，避免 forEach
过程中再触发 registry 写。

**锁规则**：所有 `m_pending_reimports` / `m_reimport_paused_until_focus`
访问都走 `m_reimport_mutex`；`onEditorFocusGained` 与
`tickReimportQueue` 都遵循"锁内只采集快照、锁外执行 importer 调用"，
避免与 `recordImportSource` 死锁（importer 内部可能在同线程回调它）。

#### 5.3.B 与原计划的差异

| 项目 | 原计划（5.3 草案） | 最终落地 | 原因 |
|------|------------------|---------|------|
| 元数据载体 | 改 `AssetFileHeader` 加 64B source-path + 8B mtime | **sidecar JSON** `<Project>/AssetRegistry/source_registry.json` | header `reserved[4]` 只有 32B，不够装路径；改长度需要改 `static_assert(sizeof(AssetFileHeader)==176)` 与 `SerializedFile::ReadHeader`，全部存量 `.zasset` 受影响；sidecar 方案保持二进制位级稳定，与 §2.1 ScriptRegistry 思路一致 |
| `reimport` 接口 | `virtual bool reimport(zasset, src) = 0;` 纯虚 | `virtual bool reimport(zasset, source, settings)` 带默认实现，default = `import(source, zasset, settings, md)` | 默认实现让全部存量 importer 零改动；只有需要 source-discovery 短路的 importer 才 override（目前没有） |
| 节流策略 | "进度条 1..50，UI 不卡" | `kReimportsPerFrame = 10`，无 UI 进度条 | 10 个 `.png` 走 `TextureImporter` 也就 ~50ms，单帧消化；进度条可以下个 PR 再加 |
| 焦点抖动防护 | 未提 | 加 `m_reimport_paused_until_focus` flag | macOS / Linux 下 ImGui 子窗口切换会触发焦点 pulse，不防会把 forEach + stat 跑爆 |
| 项目级开关 `auto_reimport` | 加 `<Project>/.zproject` 字段 | **未实现** | 目前认为 always-on 行为足够；如果 CI 流水线需要禁用再加 |
| 失败处理 | 源缺失 → log warning，保留 `.zasset` | 同左 | 一致 |

#### 5.3.C 实际改动文件（~520 行）

- ✅ `engine/Source/Editor/EditorAsset/SourceAssetRegistry.{h,cpp}`（新增）
- ✅ `engine/Source/Editor/EditorAsset/EditorAssetManager.{h,cpp}`：
  加 `m_source_registry`、`m_pending_reimports`、`m_reimport_mutex`、
  `m_reimport_paused_until_focus`；加 `onEditorFocusGained()`、
  `tickReimportQueue()`、`recordImportSource()`。
- ✅ `engine/Source/Editor/asset_pipeline/asset_importer.h`：
  加 3-arg `reimport` 默认实现。
- ✅ `engine/Source/Editor/menu/assets_menu.cpp`：
  `convertAsset` 调 `recordImportSource`。
- ✅ `engine/Source/Runtime/Function/Render/window_system.{h,cpp}`：
  加 `registerOnWindowFocusFunc` + GLFW 焦点回调 fan-out。
- ✅ `AGENTS.md` §2.10：PR-AI3 段标记 ✅ Landed，记录设计决策。
- ❌ `engine/Source/Runtime/asset/asset_file.h`：**未改**（见
  5.3.B 第一行）。

**对照 UE/Unity**：触发模型同 Unity（焦点驱动），存储模型同 UE
（路径在 sidecar，源文件不复制）。`UAutoReimportManager` 的
`FContentDirectoryMonitor` 状态机被简化掉了——ZEngine 的用户场景
很少需要"用户正在编辑时弹 PromptUser"那种细粒度交互。

**验收**（已通过）：

- 切到 Photoshop 改源 png → Alt-Tab 回编辑器 → inspector 自动
  刷新，GUID 保留，材质引用 valid。✅
- 源 png 被删 → 日志 warning，`hero.zasset` 保留（来自 importer
  默认实现里 `import` 失败的语义）。✅
- 一次焦点切回 50 个 asset → 5 帧消化（10/帧），UI 流畅。✅
- 编译验证：`cmake --build build --config Debug --target ZEditor`
  零 PR-AI3 引入的 error；预存在的 `dx12_bindless_smoke_test.obj`
  vs `vulkan_bindless_smoke_test.obj` 的 `main` LNK2005 与本 PR
  无关（PR-AI1/AI2 收尾时已记录）。✅

---

## 6. 落地顺序与互不阻塞性

| PR | 依赖 | 估算 LOC | 是否可独立 |
|---|---|---|---|
| PR-AI1（drop-target） | PR-PW2 | ~150 | ✅ |
| PR-AI2（shaders watcher） | 无（与 AI1 平行） | ~250 | ✅ |
| PR-AI3（auto-reimport） | 修改 `.zasset` 头（向后兼容路径） + importer 接口 | ~500 | ✅ |

三个 PR 之间没有顺序依赖，**优先级建议**：AI1 → AI2 → AI3，因为
AI1 用户感知最强（拖入即用），AI2 改善编辑闭环，AI3 长期价值大但
改动面也最大（动 `.zasset` 二进制格式必须仔细做向后兼容测试）。

---

## 7. 不做的事（明确边界）

1. **不重新设计 watcher**：现有 `FileSystemWatcher` 三平台都跑通
   了（已被 `m_file_watcher` / `m_data_watcher` / `m_js_watcher`
   验证），不动。
2. **不接 OS 拖出（拖一个 `.zasset` 出 ImGui 到资源管理器）**：
   这是 OLE/UTI/X11 相反方向，属性完全不同；UE 也不支持。
3. **不接非 Project window 的 OS drop**：Hierarchy / Scene view
   不接 OS 文件 drop（与 Unity 不同 —— Unity 允许直接拖 fbx 进
   Scene view 实例化）。如果以后要做，就在 PR-AI1 的基础上加一
   个 `OnSceneOsDrop` 钩子，复用 `OsDropQueue`。
4. **不做 hash-based 变更检测**（UE `FFileCache` 那套）：mtime
   足够，importer 幂等。

---

## 8. 附录：UE / Unity 关键源码定位

### 8.1 UE 拖入

```
Engine/Source/Runtime/Slate/Private/Framework/Application/SlateApplication.cpp:7277
    EDropEffect::Type FSlateApplication::OnDragEnterFiles(...)
        FExternalDragOperation::NewFiles(Files)

Engine/Source/Runtime/SlateCore/Public/Input/DragAndDrop.h:216
    class FExternalDragOperation : public FDragDropOperation

Engine/Source/Editor/ContentBrowser/Private/AssetViewWidgets.cpp:529
    FReply SAssetViewItem::OnDrop(...)

Engine/Plugins/Editor/ContentBrowser/ContentBrowserAssetDataSource/
  Source/.../ContentBrowserAssetDataSource.cpp:3188
    bool UContentBrowserAssetDataSource::HandleDragDropOnItem(...)
        GEditor->GetEditorSubsystem<UImportSubsystem>()
            ->ImportNextTick(ImportFiles, FolderPayload->GetInternalPath().ToString());

Engine/Source/Editor/UnrealEd/Private/Subsystems/ImportSubsystem.cpp:101
    UImportSubsystem::ImportNextTick → AssetTools.ImportAssets(...)
```

### 8.2 UE DirectoryWatcher

```
Engine/Source/Developer/DirectoryWatcher/Public/IDirectoryWatcher.h:44
    virtual bool RegisterDirectoryChangedCallback_Handle(
        const FString& Directory, const FDirectoryChanged& InDelegate,
        FDelegateHandle& OutHandle, uint32 Flags = 0) = 0;

Engine/Source/Developer/DirectoryWatcher/Private/Windows/
  DirectoryWatchRequestWindows.cpp:96
    ::ReadDirectoryChangesW(...)

Engine/Source/Developer/DirectoryWatcher/Private/Linux/
  DirectoryWatchRequestLinux.cpp:98
    inotify_init1 + inotify_add_watch

Engine/Source/Developer/DirectoryWatcher/Private/Mac/
  DirectoryWatchRequestMac.cpp:75
    FSEventStreamCreate + FSEventStreamStart

Engine/Source/Runtime/AssetRegistry/Private/AssetRegistry.cpp:1283
    DirectoryWatcher->RegisterDirectoryChangedCallback_Handle(
        WatchRoot, ::CreateUObject(this, &UAssetRegistryImpl::OnDirectoryChanged), ...)
```

### 8.3 UE AutoReimport

```
Engine/Source/Editor/UnrealEd/Public/AutoReimport/AutoReimportManager.h:27
    UCLASS(config=Editor) class UAutoReimportManager

Engine/Source/Editor/UnrealEd/Private/AutoReimport/AutoReimportManager.cpp:281
    if (Settings->bMonitorContentDirectories) { SetUpDirectoryMonitors(); }

Engine/Source/Editor/UnrealEd/Private/AutoReimport/ContentDirectoryMonitor.cpp:619
    void FContentDirectoryMonitor::ReimportAsset(...)
        FReimportManager::Instance()->Reimport(Asset, ...);
```

### 8.4 ZEngine 现有（编辑当前模块时主要参考）

```
engine/Source/Editor/file_system_watcher/file_system_watcher.{h,cpp}
    跨平台 watcher，三后端齐全，已被多处复用。

engine/Source/Editor/EditorAsset/EditorAssetManager.cpp:46-59
    m_file_watcher 在 <Project>/Assets/ 上的接入点，PR-AI2/AI3
    要在此处旁边加 m_shaders_watcher / 焦点回调。

engine/Source/Editor/EditorApplication/EditorApplication.cpp:693
    每帧 TickWatcher 调用点。

engine/Source/Editor/menu/assets_menu.cpp:convertAsset
    PR-PW2 已就绪的导入入口，PR-AI1 直接复用。

engine/Source/Editor/editor_drag_drop/editor_drag_drop.h
    ImGui 内部 payload 命名空间；PR-AI1 不污染此处，OS drop 走
    全局队列而非 ImGui payload。

engine/Source/Runtime/asset/asset_file.h
    AssetFileHeader 当前 176 B（"ZASS" 魔数）；PR-AI3 在尾部追加
    sourceful 字段，`AssetRegistry::scanSingleAsset` 已经按版本
    号读取，向后兼容。
```

---

## 9. 维护

实现 PR-AI1 / PR-AI2 / PR-AI3 时：

1. 把对应章节的"未接"改成"已接"，并在 §1 现状盘点的表格里更新。
2. 在 `AGENTS.md` §2.10（或新加 §2.11）记录该阶段的稳定行为，
   保证后续 session 不丢失上下文。
3. 如果实现偏离了本文档的设计（很正常），同步更新本文档，并在
   `CHANGELOG.md` 留一条 entry 指向本 doc 的提交 hash。
