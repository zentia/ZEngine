# ZEngine TypeScript 脚本系统设计

> 目标：让用户在 `<ProjectRoot>/Scripts/**/*.ts` 中编写 TypeScript 代码，
> Editor 和 Runtime（独立游戏可执行文件）都能加载并执行同一份代码，
> 行为对齐 Unity 的 `Assets/Scripts/*.cs`。

本文档对应需求场景：
> "I:\\ZEngineDemo 这个项目目录，I:\\ZEngineDemo\\Scripts 这个目录下面是 TS 目录，
> 想让这个目录的代码参考 Unity 代码一样，可以在 Editor 和 Runtime 加载，该如何设计？"

---

## 1. 现状与可复用基础设施

调研结论（read-only survey）：

| 设施 | 状态 | 路径 |
|---|---|---|
| `ProjectInfo`（项目根 + Assets 子目录） | ✅ 已有，`content_dir = "Assets"` 默认值 | `engine/Source/Runtime/project/ProjectInfo.{h,cpp}` |
| `--project` 启动参数 | ✅ 已有，`CommandSystem` 解析后切 cwd | `engine/Source/Runtime/Function/command/CommandSystem.cpp` |
| Launcher → 启动 ZEditor 进程 | ✅ 已有 | `engine/Source/Launcher/ui/launcher_ui.cpp` |
| `AssetManager`（Editor + Runtime 共用） | ✅ 已有 | `engine/Source/Runtime/Resource/Asset/AssetManager.{h,cpp}` |
| 反射 (`TypeOf<>` / REGISTER_CLASS / Transfer) | ✅ 已有 | `engine/Source/Runtime/BaseClasses/` |
| **puerts 集成（QuickJS 后端）** | ✅ **已编译并冒烟通过** | `engine/3rdparty/puerts/` |
| 文件监听 (`FileSystemWatcher`) | ✅ 已有 | （survey 报告中已确认） |
| TypeScript 编译器 | ❌ **缺失**（puerts 自带 `PuertsEditor` 是 UE-only，没编进 ZEngine） | — |
| `ScriptAsset` / `MonoBehaviour` 类似物 | ❌ **缺失** | — |
| `.ts` 接入 AssetManager 扩展名表 | ❌ **缺失** | — |

**关键发现**：puerts 已经能跑 JS。所以战略是 **TS → JS（外部 tsc）→ puerts 加载**，不去自己写 TS 编译器。

---

## 2. Unity 的对照参考

ZEngine 设计要镜像的 Unity 行为（不是抄实现，是抄契约）：

| Unity 行为 | ZEngine 对应 |
|---|---|
| 把 `.cs` 文件丢进 `Assets/Scripts/` | 把 `.ts` 文件丢进 `<ProjectRoot>/Scripts/` |
| Editor 编译成 `Library/ScriptAssemblies/Assembly-CSharp.dll` | 外部 `tsc` 编译成 `<ProjectRoot>/Intermediate/Scripts/*.js` |
| Editor 和独立 Player 共享同一个 dll | Editor 和 Runtime 共享同一份编译产物目录 |
| 把 `MonoBehaviour` 子类拖到 GameObject 上 = AddComponent | 把导出 `class Foo extends Behaviour {}` 的脚本绑到 `TypeScriptComponent` 上 |
| `.cs.meta` GUID + ScriptAsset 资产 | **无 meta**：`ScriptRegistry` 集中映射 path↔GUID（参考 UE Redirector） |
| Project 视图能看到 `.cs`，双击进 IDE | Project 视图能看到 `.ts`，双击进 VS Code |
| Hot Reload：保存 → 重新编译 → 重启 PlayMode | 保存 → tsc watch 自动 emit → ScriptingEngine reload |

---

## 3. 整体目录约定

每个 ZEngine 项目（如 `I:\ZEngineDemo\`）布局：

```
I:\ZEngineDemo\
├─ ZEngineDemo.zproject       # 已有：项目入口
├─ Assets\                     # 已有：content_dir，存 .zasset 二进制资产
├─ Scripts\                    # **本设计新增**：用户 .ts 源码（项目根下平级，UE 风格）
│  ├─ PlayerController.ts
│  └─ EnemySpawner.ts
├─ Intermediate\Scripts\       # tsc 输出，**.gitignore 不入库**
│  ├─ PlayerController.js
│  ├─ PlayerController.js.map
│  └─ EnemySpawner.js
├─ tsconfig.json               # 由引擎首次启动时生成（如果不存在）
└─ package.json                # 由引擎首次启动时生成（如果不存在）
```

> **目录决策（P1 已落地）**：`Scripts/` 放在 `<ProjectRoot>/` 下，与 `Assets/` **平级**。
> 这与已存在的 `I:\ZEngineDemo\Scripts\` 保持一致，对齐 UE 的 `<Project>/Source/`。
> 在 P2 中，ScriptAssetImporter 会把 `Scripts/` 注册成 Project 窗口的额外扫描根，让
> .ts 文件像 Assets 下的 zasset 一样在项目视图中出现。

`tsconfig.json` 自动生成内容（参考 Unity 的 `Assembly-CSharp.csproj` 自动生成）：

```json
{
  "compilerOptions": {
    "target": "es2020",
    "module": "commonjs",
    "outDir": "./Intermediate/Scripts",
    "rootDir": "./Scripts",
    "sourceMap": true,
    "experimentalDecorators": true,
    "strict": false
  },
  "include": ["Scripts/**/*.ts", "Intermediate/Typings/**/*.d.ts"]
}
```

---

## 4. 模块拆分（六阶段，每阶段独立可编译）

### Phase 1 — ProjectInfo 扩展（最小改动）

**文件**：`engine/Source/Runtime/project/ProjectInfo.{h,cpp}`

新增字段：
```cpp
std::string scripts_dir       {"Scripts"};        // 相对 content_dir
std::string intermediate_dir  {"Intermediate"};   // 相对 project_root
```

新增 API：
```cpp
std::filesystem::path getScriptsRoot()             const; // = project_path / Assets / Scripts
std::filesystem::path getIntermediateScriptsRoot() const; // = project_path / Intermediate / Scripts
std::filesystem::path getTsConfigPath()            const; // = project_path / tsconfig.json
```

`ProjectInfo::loadFromFile` 末尾追加 `ensureScriptsScaffold()`：如果 `Scripts/` 或 `tsconfig.json` 不存在则按模板生成。

### Phase 2 — ScriptAsset + ScriptRegistry（无 meta 文件，UE 风格）

> **关键设计原则**：ZEngine **不使用 `.meta` 侧车文件**（参见 `AGENTS.md` §2.1）。
> 这一节把 Unity 的 `.cs.meta` 模式改造成 UE 风格的"约定优于配置 + 集中式
> Redirector 注册表"。

#### 2.1 为什么不用 .meta，UE 是怎么做的

| 方案 | 形态 | 主要问题 |
|---|---|---|
| Unity `.cs.meta` | 每个 `.cs` 旁一个 YAML，存 GUID | 文件数翻倍；用户 git add 时漏 .meta 导致引用断裂；外部 IDE 看到一堆奇怪文件 |
| UE C++ 类 | 完全无侧车，编译期靠 `UCLASS()` + UHT 生成代码 | 不可移植到 .ts（我们没有 UHT）|
| UE Blueprint `.uasset` | GUID 直接埋在 .uasset 二进制里，靠 `UObjectRedirector`（独立小 .uasset）保留旧引用 | 这个思路 **可以** 移植到 .ts |
| **本设计** | `.ts` 旁无任何文件；GUID 集中到 `<Project>/AssetRegistry/script_registry.json`（**入库**） | 一个 JSON = 全部映射；用户重命名 .ts 时 ScriptRegistry 原地改路径，GUID 不变 |

#### 2.2 数据结构

**新文件 1**：`engine/Source/Runtime/Resource/Script/ScriptAsset.{h,cpp}`

`ScriptAsset` 是 Object 派生的**纯内存对象**（不写盘，所以**没有** `.zasset`），
由 ScriptRegistry 在扫到一个 .ts 时构造，作为 `loadAsset<ScriptAsset>(guid)` 的返回值：

```cpp
class ScriptAsset : public Object {
    REGISTER_CLASS(ScriptAsset);
public:
    // Stable identity, generated from a deterministic hash of the relative
    // path on first discovery. Survives renames via ScriptRegistry remap.
    Guid          m_guid;

    // Path is relative to <Project>/. Always forward-slash, lower-cased on
    // case-insensitive platforms (Windows) for stable comparison.
    eastl::string m_source_rel_path;     // "Scripts/PlayerController.ts"
    eastl::string m_compiled_rel_path;   // "Intermediate/Scripts/PlayerController.js"

    // Parsed from `export (default )?class X extends (Behaviour|Component)`.
    // Empty if the file doesn't declare one (e.g. utility module).
    eastl::string m_default_class_name;

    // mtime in ns — drives "skip unchanged" decisions in P3 incremental compile
    // and P6 hot-reload.
    int64_t       m_source_mtime_ns = 0;

    template<typename TF> void Transfer(TF&);  // for in-memory serialise (PPtr)
};
```

**新文件 2**：`engine/Source/Editor/asset_pipeline/ScriptRegistry.{h,cpp}`

```cpp
// Centralised path<->guid mapping for all .ts files in the project.
// Persisted to <Project>/AssetRegistry/script_registry.json (checked into VCS).
//
// This is the SINGLE source of truth for "what script assets exist".
// Reading the registry is cheap (JSON load on Editor startup); writing
// happens on file events from FileSystemWatcher.
class ScriptRegistry : public IEngineSystem {
public:
    bool Initialize();   // load JSON, kick off full-scan of <Project>/Scripts/
    void Shutdown();     // flush JSON

    // Lookup APIs (used by AssetManager, ProjectWindow, P5 TypeScriptComponent)
    ScriptAsset* findByGuid(const Guid& guid) const;
    ScriptAsset* findByPath(const eastl::string& rel_path) const;
    std::vector<ScriptAsset*> getAll() const;

    // FileSystemWatcher callbacks — the only mutating entry points
    void onFileAdded   (const std::filesystem::path& abs_path);
    void onFileRemoved (const std::filesystem::path& abs_path);
    void onFileRenamed (const std::filesystem::path& old_abs,
                        const std::filesystem::path& new_abs);  // <-- preserves GUID
    void onFileModified(const std::filesystem::path& abs_path); // mtime + class re-parse

private:
    bool loadFromDisk();
    bool saveToDisk();   // atomic write via temp + rename
    void scanScriptsRoot(const std::filesystem::path& scripts_root);

    // Header-parser: reads first ~100 lines, extracts default class name.
    // Lightweight regex match, NOT a full TS parser.
    static eastl::string parseDefaultClassName(const std::filesystem::path& abs_ts);

    std::unordered_map<Guid, std::unique_ptr<ScriptAsset>>      m_by_guid;
    std::unordered_map<eastl::string, ScriptAsset*>             m_by_path;
    std::filesystem::path                                       m_registry_json;
    std::mutex                                                  m_mutex;
};
```

#### 2.3 script_registry.json 形态

```json
{
  "version": 1,
  "entries": [
    {
      "guid":  "1f3a7e2c8b1d4e2a9c7d6f5b4a3210ef",
      "path":  "Scripts/Player/PlayerController.ts",
      "class": "PlayerController",
      "mtime": 1715843921000000000
    },
    { ... }
  ]
}
```

- `guid` 在文件**首次发现**时生成（`Hash64(rel_path)` 的两半拼成 128 位）。
  之所以用 deterministic hash 而不是 UUIDv4：用户 `git clone` 一个老项目、
  registry 文件丢失时，重新扫描得到的 GUID 跟历史一致，旧场景里的引用
  不会失效。
- 用户**重命名** `.ts`（在 IDE 或 Project 窗口里）：FileSystemWatcher
  `onFileRenamed` 把 `path` 字段就地改成新路径，**GUID 保持不变** —— 这就是
  UE Redirector 的等价物，只是没有 .uasset，全部在一个 JSON 里。
- 用户**手动删除** `.ts`：entry 被移除，但 GUID 保留在一个 `tombstones`
  数组里 N 天（默认 30），方便撤销 / 跨人误删保护。（P2 暂不实现 tombstone，
  推到 P6 一起做。）

#### 2.4 接入 AssetManager

**改动**：`engine/Source/Runtime/Resource/Asset/AssetManager.h:65`（`loadAsset<T>`）。

`.ts` 不是 `.zasset` 也不是 `.json`，单独一个分支：

```cpp
template<typename AssetType>
AssetType* loadAsset(const eastl::string& asset_url) const {
    std::filesystem::path p = getFullPath(asset_url);
    auto ext = lower(p.extension().string());
    if (ext == ".ts" || ext == ".js") {
        // Delegate to ScriptRegistry; AssetManager is just a facade here.
        return static_cast<AssetType*>(
            GET_SYSTEM(ScriptRegistry)->findByPath(asset_url));
    }
    // ... existing .zasset / .json branches unchanged ...
}
```

`ScriptRegistry` 是一个 `IEngineSystem`，**Editor + Runtime 都 GET_SYSTEM**
（这是关键 —— Runtime 也需要按 GUID 查 .js 模块路径）。Runtime 加载时
不会扫盘，只读 `script_registry.json`（已经被 P3 的 build pipeline
打包进 cooked content）。

#### 2.5 Project 窗口接入

**改动**：`engine/Source/Editor/editor_window/project_window/project_window.cpp`

现状：只扫 `getProjectContent()`（即 `<Project>/Assets/`）。

改动：在树根**并列**新增一个根节点 `Scripts`，指向 `getScriptsRoot()`，
同样使用现有 `EditorFileNode` 递归。后缀过滤白名单加上 `.ts` / `.tsx`。
双击 `.ts` 触发 `editor_utility::OpenInExternalEditor()`（已有 API）。

`EditorFileNode::loadGuidFromFile` 当前从 .zasset header 读 GUID；对
`.ts` 不走这条路径，而是 `m_guid = ScriptRegistry::findByPath(rel)->m_guid`。

#### 2.6 P2 验收

1. `cmake --build` 通过，0 errors / 0 ProjectInfo-related warnings。✅
2. 启动 ZEditor 加载 `I:\ZEngineDemo`：
   - 在 `<Project>/Scripts/` 下手动放一个 `Hello.ts`（`export class Hello {}`）。
   - Project 窗口出现 `Scripts/Hello.ts` 节点。
   - `<Project>/AssetRegistry/script_registry.json` 自动生成，
     包含一条 `Hello.ts` 的 entry，`class: "Hello"`，`content_hash` 字段非空。✅
3. 在 IDE 里把 `Hello.ts` 重命名为 `Hi.ts`（要求：上次 rescan 已经为该条目写入 `content_hash`）：
   - Project 窗口节点变成 `Hi.ts`。
   - registry JSON 里 **GUID 保持不变**，只 `path` 变了。✅（实测 Hi.ts→Hi2.ts，GUID `ae57a4b9...` 保留）
4. 删除某个 `.ts`：节点消失，registry entry 消失。✅
5. 第二次启动 ZEditor：registry SHA256 不变（幂等）。✅

**已知限制（cold-start rename detection）**：rename 检测依赖
旧条目里已经存在 `content_hash` 字段。如果一个文件**首次被 ScriptRegistry
扫描时就已经被改过名**（registry 中只有路径，没有 hash），则 rescan 会
按"删 + 增"处理，新条目会获得新 GUID。这只在版本升级（旧 registry 缺
`content_hash` 字段）或第一次扫描的同一会话内发生，正常使用不会触发。
P6 引入 `FileSystemWatcher` 在 IDE rename 事件上同步更新 path 后即可消除。

#### 2.7 P2 不做的事

- **不**生成 .ts.meta 之类的侧车（违反 §2.1）。
- **不**编译 .ts → .js（这是 P3）。
- **不**做 puerts 加载（这是 P4）。
- **不**做 Inspector 显示（这是 P5）。
- **不**做 Editor 内重命名 / Move（依赖外部 IDE 完成；P6 加 Editor 内 rename）。

### Phase 3 — TypeScriptCompiler（外部 tsc 子进程包装） ✅

**新文件**：`engine/Source/Editor/Scripting/TypeScriptCompiler.{h,cpp}`

> Editor-only 模块。Runtime 直接读 `Intermediate/Scripts/*.js`，不需要编译能力。

职责：
1. 启动时检查 `node` / `tsc` 是否在 PATH（不在则提示用户安装一次性，后续静默）
2. 起一个后台线程执行 `tsc --watch -p <projectRoot>`，把 stdout/stderr 转发到 ZEngine 控制台
3. 监听 `Intermediate/Scripts/` 的 `.js` 文件变化（用现有 `FileSystemWatcher`），通知 `ScriptingEngine::ReloadModule(path)`

**实现细节**（已落地）：
- 工具链探测顺序：① `<project>/node_modules/.bin/tsc(.cmd)`（项目本地、版本受 `package.json` 钉死，**优先**） ② 系统 PATH 上的 `tsc(.cmd|.exe)` ③ 都没有则降级。
- Windows 子进程通过 `cmd.exe /c "<tsc> --watch --pretty false -p <project>"` 启动，stdout/stderr 用匿名管道捕获，后台 reader 线程按行切分推入主线程队列；POSIX 用 `fork+execlp`+`pipe`+非阻塞 `read`。
- `--pretty false`：抑制 ANSI 颜色 / unicode 框线，让日志可靠落到 ZEngine 控制台/文件。
- IEngineSystem 的 init phase 取 `PostInit`（依赖 ProjectInfo），每帧由 `Editor::run()` 主循环调一次 `Tick()` 驱动日志和 watcher 队列消费。
- `FileSystemWatcher` 原本硬编码只监听 `.zasset`。本期把过滤器改为可注入扩展名集合（默认仍 `{".zasset"}` 保持向后兼容），TypeScriptCompiler 注入 `{".js"}` 来收编 tsc 的产物。
- `JsChangeHandler` 留作 P4 钩子；P3 只把变化作 `LOG_INFO` 输出。

**降级策略**：如果用户没装 node/tsc，`TypeScriptCompiler` 进入"诊断模式"——只把 `.ts` 复制为 `.js`（删除类型注解），并在 Editor UI 顶部显示黄色横幅提示安装 tsc。这保证 demo 项目"开箱即跑"。
> P3 当前实现：探测失败时打 LOG_WARNING + `m_degraded=true`，**不**做 .ts→.js 复制（容易误导用户以为编译成功）。UI 横幅留到 P5 一起做。降级模式下引擎正常启动，只是没有脚本执行。

#### P3 验收

1. `cmake --build` 通过：✅
2. 在 demo 项目 `npm install` 后，启动 ZEditor：
   - `[ZTSC] Resolved toolchain: tsc=I:/ZEngineDemo/node_modules/.bin/tsc.cmd, node=<not needed>`
   - `[ZTSC] tsc --watch spawned (pid=...)`
   - `[ZTSC] Watching JS output directory: I:/ZEngineDemo/Intermediate/Scripts`
   - `[ZTSC] [tsc] X:XX:XX PM - Starting compilation in watch mode...` ✅
3. `Intermediate/Scripts/Hello.js` + `Util.js` 自动生成 ✅
4. 编辑 `Scripts/Hello.ts` 保存 → 几秒内 `[ZTSC] [tsc] File change detected. Starting incremental compilation...` 出现，`Hello.js` mtime 更新 ✅
5. `[ZTSC] Compiled module updated: .../Hello.js` 由 FileSystemWatcher 触发 ✅（注意：Win32 ReadDirectoryChangesW 会对单次写产生多条事件，重复触发 OK，去抖留到 P4 ScriptingEngine 边界做。）
6. tsc 类型错误（如 `Cannot find name 'Behaviour'`）会一字不漏出现在 ZEngine 日志里，方便用户诊断 ✅

#### P3 不做的事
- **不**实际执行 .js（这是 P4 ScriptingEngine 的职责）
- **不**做 fallback `.ts → .js` 转换（设计文档原描述里的"诊断模式"被推迟）
- **不**做事件去抖 / 多次写合并（在 P4 ReloadModule 入口做更合理）
- **不**做 UI 横幅（P5）



### Phase 4 — ScriptingEngine（puerts 包装，Editor + Runtime 共用）  ✅ 已完成

**实际落地路径**：合并到既有的 `engine/Source/Runtime/Scripting/ScriptingManager.{h,cpp}` —— 而非按设计草稿引入新 `ScriptingEngine` 类。`ScriptingManager` 在 P4 之前就已经持有 `ScriptEnv` 并完成了 QuickJS bootstrap 冒烟测试，再额外引入一个 IEngineSystem 只会带来重复的依赖图与额外样板，所以直接在 `ScriptingManager` 上补全本节描述的 API。

**实际公共 API**（与原草稿等价）：

```cpp
class ScriptingManager : public IEngineSystem {
public:
    SystemInitPhase  GetInitPhase() const override { return SystemInitPhase::Core; }
    std::vector<std::type_index> GetDependencies() const override;  // {ProjectInfo}

    bool BindToProject(const ProjectInfo&);            // js_root = Intermediate/Scripts/
    bool LoadModule(const std::string& module_id);
    bool ReloadModule(const std::string& module_id);
    void UnloadModule(const std::string& module_id);
    bool IsModuleLoaded(const std::string& module_id) const;
    bool InvokeExportedFunction(const std::string& module_id, const char* fn_name);
    std::string PathToModuleId(const std::filesystem::path& abs_js_path) const;
    void Tick();
    ScriptEnv*  GetEnv();
    const std::filesystem::path& GetJsRoot() const;
};
```

**`CreateInstance/InvokeMethod` 推迟到 P5**：原草稿里这两个 API 是为 `TypeScriptComponent` 准备的，P4 阶段没有调用方，强行实现等于盲写。已经预留 `GetEnv()` + 现成的 `pesapi_*` 路径，P5 真正写 Component 时再补即可。

**module 加载策略（QuickJS 没有 require）**：QuickJS 后端在 puerts 中没有 `require/CommonJS`，所以 `LoadModule` 内部把 `.js` 包成一次性 IIFE：

```js
(function(module, exports){
  /* 文件内容（tsconfig target=commonjs，所以是 exports.Foo = ... 风格） */
  ;return module.exports;
})
```

执行后把 `module.exports` 存进 `m_Modules: unordered_map<string, pesapi_value_ref>`。`ReloadModule = Unload + Load`，缓存按 module_id 替换；`UnloadModule` 释放 ref 即可，QuickJS GC 会回收挂在 exports 下的所有对象（除非有逃逸到 globalThis 的引用，那是脚本作者自己的问题）。

**console / Debug 全局**：`ScriptEnv` 构造时调用 `InstallConsoleAndDebugGlobals()`：
1. `pesapi_create_function` 注册 3 个 native sink，对应 ZScripting 日志的 info / warning / error 三档；
2. `pesapi_eval` 一段 ~30 行的 JS shim，把 `globalThis.console.{log,info,debug,warn,error}` 与 `globalThis.Debug.{Log,LogWarning,LogError}` 都接到这 3 个 sink，参数走 `Array.prototype.map(String).join(' ')` 拼接。

这样不论用户用 `console.log`、`console.warn`、还是 `Debug.Log`，最终都汇聚到 `[ZScripting]` 日志类别，与 C++ 端的 `LOG_INFO(ZScripting, ...)` 共用同一条管线。

**启动时机（已实现）**：
- **Editor**：`ScriptingManager::Initialize()`（phase Core）创建 QuickJS env，随后通过 `BindToProject(*GET_SYSTEM(ProjectInfo))` 把 js_root 指向 `Intermediate/Scripts/`。`Editor::Initialize()`（phase PostInit，跑在 `TypeScriptCompiler` 之后）再做两件事：
  1. 把 `TypeScriptCompiler::SetOnJsModuleChanged` 接到 ScriptingManager — 已加载模块走 `ReloadModule`，未加载模块走 `LoadModule`，事件 200ms 去抖（Win32 ReadDirectoryChangesW 一次写常发多条 ADD/MODIFIED）。
  2. 扫一次 `js_root/*.js`，把已经存在的 `.js` 主动 `LoadModule` 一遍 — 这样冷启动（Intermediate 内容已是新的、tsc 不会重新发事件）也能执行模块；用户首次保存 `.ts` 走 watcher 路径，热重载与冷启动共享同一套缓存。
- **Runtime**：等 P5/P6 真正引入 Player.exe 路径时再接上。架构上 `ScriptingManager` 已经是 `Runtime/` 下的 system，无需挪动。

**MSVC 兼容性补丁**：QuickJS 的 `quickjs/libbf.c` 在 `__AVX2__` 路径里大量使用 GCC 风格的 `__m256d` 运算符重载（`a + b`、`a * b`、`(x - y)`），MSVC 不支持。父级 `engine/CMakeLists.txt` 全局设了 `/arch:AVX2`，使得 MSVC 把 `__AVX2__` 预定义出来，触发上述代码。修复方式见 `engine/3rdparty/puerts/unity/native/papi-quickjs/CMakeLists.txt`：在该 target 末尾给 `quickjs/libbf.c` 单独追加 `/arch:SSE2`，命令行最后一个 `/arch:` 优先，因此 `__AVX2__` 不再被预定义；libbf 走标量回退路径，功能上无差别（AVX2 代码只在 ≥100-limb 大整数乘法时才会被调用，脚本场景不会触发）。

**P4 验收（实测）**：

冒烟脚本 `I:\ZEngineDemo\Scripts\P4Smoke.ts`：

```ts
console.log("[P4Smoke] hi from TypeScript module");
console.warn("[P4Smoke] console.warn works");
Debug.Log("[P4Smoke] Debug.Log works");
Debug.LogWarning("[P4Smoke] Debug.LogWarning works");
export const P4_SMOKE_OK: boolean = true;
```

启动 ZEditor 加载 demo 项目后，BqLog 中观察到：

```
[ZScripting] startup-loading existing module: P4Smoke
[I][ZScripting] [P4Smoke] hi from TypeScript module
[I][ZScripting] [P4Smoke] console.info works
[W][ZScripting] [P4Smoke] console.warn works
[I][ZScripting] [P4Smoke] Debug.Log works
[W][ZScripting] [P4Smoke] Debug.LogWarning works
[ZScripting] module loaded: P4Smoke (from .../P4Smoke.js)
```

热重载也已验证：保存 `.ts` 后 ~600ms 内 `[ZScripting] hot-reloading module: P4Smoke` → `module unloaded: P4Smoke` → 重新执行模块顶层 → 同一组 `[P4Smoke] ...` 日志再次出现。



### Phase 5 — TypeScriptComponent（Behaviour 基类 + 可绑定到 GameObject）

**新文件**：`engine/Source/Runtime/Function/Framework/Component/Script/TypeScriptComponent.{h,cpp}`

Unity 的 `MonoBehaviour` 在 ZEngine 的对应物。

序列化字段：
```cpp
class TypeScriptComponent : public Component
{
    DECLARE_SERIALIZED_OBJECT(TypeScriptComponent, Component)

    // 关联的 ScriptAsset（用 PPtr，Inspector 拖拽赋值）
    PPtr<ScriptAsset> m_script;

    // 可序列化的 public 字段（Unity 的 SerializedField），由反射在 Inspector 编辑
    // Phase 5 暂用 string→string map；Phase 7 升级为强类型
    std::map<std::string, std::string> m_serialized_fields;

    // ----- runtime only（不序列化）-----
    JSObjectHandle m_js_instance;
    bool           m_awake_called = false;

    // 生命周期
    void Awake()  override;     // → 调 puerts 的 OnAwake
    void Start()  override;     // → OnStart
    void Update(float dt) override;
    void OnDestroy() override;
};
```

UI 接入：
- **Hierarchy / GameObject 右键 → Add Component → TypeScript Behaviour**
- **Inspector**：显示 "Script: [拖一个 ScriptAsset]" + 该脚本暴露的字段
- **Project 窗口**：双击 `.ts` 用 `editor_utility::OpenInExternalEditor()` 启 VS Code

#### P5 实际落地与原始草稿的差异

完成 P5 后，实现与上面 425-450 行的草稿有几处主动调整，记录在这里方便后续维护：

1. **不依赖 `PPtr<ScriptAsset>`**：`ScriptAsset` 至今没有走 `ObjectManager`，改成把 32 字符 GUID 直接作为 `eastl::string m_script_guid` 序列化，运行期再用 `ScriptRegistry::findByGuid` 解析。这等价于 Unity 序列化 `MonoBehaviour.m_Script.guid` 的形态，但省掉一层 ObjectManager 注册。
2. **`m_serialized_fields` 推迟到 P7**：草稿里的 `std::map<string,string>` 暂未实现，因为 inspector 当前没有 map 类型 drawer。P5 只序列化 `m_script_guid` + `m_class_name`（class 名 override，留空则取 `ScriptAsset::m_default_class_name`）。
3. **生命周期挂在 ZEngine 的 `Component` 钩子上**：ZEngine 没有 Unity 的 `Awake/Start/OnEnable/Update/OnDestroy` 五连，只有 `postLoadResource` 和 `tick`。映射如下：
   - `postLoadResource(GameObject*)` → JS 的 `OnAwake` + `OnStart`（顺序连发）
   - `tick(float dt)` → JS 的 `OnUpdate(dt)`
   - `~TypeScriptComponent()` → JS 的 `OnDestroy`，随后 `pesapi_release_value_ref`
4. **JS 实例创建走 `__zNewInstance` shim**：pesapi 没有 `new` 操作符，所以 `ScriptEnv::CreateInstance` 实际是 C++ 端拿到 ctor `pesapi_value` 后调用 JS 端的 `__zNewInstance(ctor)`（`function(ctor){ return new ctor(); }`），由我们在 console/Debug shim 旁边一起注入。
5. **`globalThis.Behaviour` 占位类**：用户脚本 `class X extends Behaviour` 在 P5 阶段还没有真正的 binding，所以 shim 里塞了一个空 `Behaviour` 类（`OnAwake/OnStart/OnUpdate/OnDestroy` 全部空实现），让 JS 端 `extends Behaviour` 能跑通。P7 真做引擎反射注入时把它替换掉即可。
6. **`Intermediate/Typings/zengine.d.ts` 引擎自动生成**：tsconfig 的 `include` 已经覆盖 `Intermediate/Typings/**/*.d.ts`，`ProjectInfo::ensureScriptsScaffold` 每次开项目都会**覆盖写**这份 .d.ts（位于 gitignored 的 `Intermediate/`，引擎拥有写权），声明 `Behaviour`/`Debug`/`console`/`__zNewInstance`，让 `tsc` 不再报 `Cannot find name 'Behaviour'`。
7. **Inspector 接入最小可用版本**：原草稿要求“GameObject 右键 → Add Component → TypeScript Behaviour”菜单 + 拖拽。P5 只做了 Inspector 底部一个 “Add TypeScript Behaviour...” 按钮 + Selectable 弹窗，里面列出所有 `m_default_class_name` 非空的 `ScriptAsset`。点一个就 `MemoryManager::CreateObject<TypeScriptComponent>` + `SetScriptGuid` + `addComponent` + `postLoadResource`。Hierarchy 右键菜单与拖拽留给 P7/P8。
8. **Edit-mode tick**：Component 派发 `tick` 时在 Editor 模式下要求类型名出现在 `g_editorTickComponentTypes` 里，已在 `EditorApplication::Initialize` 加 `registerEdtorTickComponent("TypeScriptComponent")`。这是 Unity `[ExecuteAlways]` 的等价物，让 OnUpdate 在不进入 Play 模式时也能触发。
9. **`CreateInstance/InvokeMethod` 是 P4 推迟过来的**：从 P4 计划里抠出来一并做掉，避免 P5 还要回头补 ScriptEnv 接口。`ScriptingManager::{CreateInstance,DestroyInstance,InvokeInstanceMethod,InvokeInstanceMethodNumber}` 全部是 ScriptEnv 的薄转发；`pesapi_value_ref` 由调用方持有，必须 `DestroyInstance` 释放一次。

#### P5 验收（实测）

`I:\ZEngineDemo` 上的冒烟流程：

1. `cmake --build build --config Debug --target ZEditor` 通过：✅
2. 启动 ZEditor 加载 demo 项目：
   - 自动写出 `I:/ZEngineDemo/Intermediate/Typings/zengine.d.ts`（1.5KB）
   - tsc --watch 编译 `Hello.ts` 报 `Found 0 errors`（之前的 `TS2304: Cannot find name 'Behaviour'` 消失）
   - 启动期日志看到 `[ZScripting] module loaded: Hello (from .../Intermediate/Scripts/Hello.js)`，没有 `ReferenceError: Behaviour is not defined`
3. 在 Inspector 选中场景里任意 GameObject，点底部 “Add TypeScript Behaviour...”，从弹窗选 “Hello”：
   - 控制台依次出现 `Hello.OnAwake`、`Hello.OnStart`
   - 之后每 60 帧出现一次 `Hello.OnUpdate dt=... tick=...`
4. 编辑 `Hello.ts` 改输出文本并保存：~600ms 内看到 `module unloaded` → `module loaded`，紧接着新的 `OnAwake`/`OnStart`/`OnUpdate` 输出（热重载继承自 P3+P4，P5 只是搭顺风车确认仍然好用）。



### Phase 6 — Hot Reload + Project 窗口联动

整合：
- `TypeScriptCompiler` 检测到 `.js` 变更 → 通过事件通知 `ScriptingEngine::ReloadModule`
- `ScriptingEngine::ReloadModule` 找出所有挂在场景 GameObject 上、用了这个 module 的 `TypeScriptComponent`，调 `OnDestroy`，重建 instance，调 `OnAwake/OnStart`
- Project 窗口已有的 `EditorFileNode` 自动看到 `.ts`（已是文件系统扫描），加一个 icon 区分

#### P6 实际落地

1. **Reload 通知改为 Observer 模式（不是反向遍历场景）**：原始草稿写的是"`ReloadModule` 找出场景里所有挂这个 module 的 `TypeScriptComponent`"。这要求 `ScriptingManager`（Runtime 层）能枚举 `Level::m_gobjects` + 每个 GameObject 的 component list 并 `dynamic_cast<TypeScriptComponent*>`，一来层级倒挂（Runtime 不该依赖 World/Level 的具体实现细节做反射式扫描），二来在 Edit-mode 多 Level / 多 World 场景下也撑不住。改为：`ScriptingManager::AddModuleReloadObserver(fn)/RemoveModuleReloadObserver(token)`，每个 `TypeScriptComponent` 在 `BindAndAwake` 成功后订阅一次（无视具体 module id），在回调里自己按 `m_module_id` 过滤；`TearDown` / dtor 取消订阅。这样：
   - 没有跨层依赖；
   - 任意 Component 类型（不只 `TypeScriptComponent`）将来想吃 reload 事件，都能直接订阅，零侵入；
   - 重入安全（`NotifyModuleReloaded` 在锁内对 observer 列表做 `vector` snapshot，再无锁迭代）；
   - 没绑定的 `TypeScriptComponent` 不付钱（订阅在第一次 `CreateInstance` 之后才发生）。

2. **Reload 等于 stateless rebuild**：观察者收到匹配的 `module_id` 时只做 `BindAndAwake()`，而 `BindAndAwake` 自带 `if (m_js_instance) TearDown();` 前缀。一次调用同时完成"OnDestroy 旧实例 → DestroyInstance → CreateInstance 新实例 → OnAwake → OnStart"四步，与 Unity `MonoBehaviour` 重新编译时的行为对齐：**字段默认值会丢，跨 reload 状态不保留**。`m_serialized_fields` 字符串 map（草稿里设想的状态恢复机制）到 P7 才做。

3. **触发链**：`tsc --watch` 写出 `.js` → `FileSystemWatcher`（ReadDirectoryChangesW）→ `TypeScriptCompiler::Tick` 主线程 drain → `m_on_js_changed` 回调 → `EditorApplication` 200ms debounce → `ScriptingManager::ReloadModule` → `ScriptEnv::ReloadModule`（unload + load）→ **新增**：成功路径上调用 `NotifyModuleReloaded(module_id)` → 所有订阅者按需 rebuild。注意只有 `ReloadModule` 走这条路，`LoadModule`（首次 / 冷启动批量 load）不发通知，因为那时还没有 component 需要 rebuild。

4. **双击 `.ts` 在 VSCode 打开**：在 `EditorUtility` 接口里新增 `openInExternalEditor(eastl::string path)`，Windows 实现优先级：`ZENGINE_EXTERNAL_EDITOR` 环境变量 → `ShellExecuteW(L"open", L"code", "<file>")`（依赖 PATH + PATHEXT 让 Windows 解析 `code.cmd`）→ 系统默认关联程序。macOS 对应 `open -a "Visual Studio Code"`，再退到默认 `open`。`project_window.cpp` 里原本 P2 留下的"双击 .ts/.tsx/.js → `revealInFinder`"占位被替换成 `openInExternalEditor`，并把 `.json` 也加进白名单（手编 `.zproject` 时常用）。其它资产类型（场景/Prefab 等）维持现状不动——它们走的是单击 → `EditorSceneManager::onAssetSelected` 的另一条路。

5. **Icon 区分推迟**：草稿提到给 `.ts` 一个独立 icon，但 ZEngine Project 窗口当前对所有非 folder leaf 是同一个图标（`m_file_type` 字符串只用于双击分发，不参与渲染），加 icon 需要扩展资源管线本身，不在 P6 必要交付里，挪到后续 polish。

#### P6 验收（实测）

1. **Build**：增量 Debug 0 错误（`ScriptingManager.{h,cpp}` 加 observer API；`TypeScriptComponent.{h,cpp}` 加订阅 + `OnModuleReloaded`；`editor_utility.{h, windows.cpp, macos.cpp}` 加 `openInExternalEditor`；`project_window.cpp` 改双击分发）。
2. **冷启动批量 load 不触发 reload 通知**：日志里启动期间 `LoadModule(P4Smoke/Util/Hello)` 出现，但没有 `hot-reloading module:` 那一行配套——观察者也确实不应该在那时就 rebuild（彼时 component 自己还没走完 `BindAndAwake`）。
3. **Hot reload 端到端**：编辑器运行中改 `Hello.ts`（在 `OnUpdate` 文案里加 `HOT_RELOAD_MARKER_V2`），保存。BqLog `[ZScripting]` 类目 ~3 秒内依次出现：
   - `[ZTSC] Compiled module updated: I:/ZEngineDemo/Intermediate/Scripts/Hello.js`
   - `[ZScripting] hot-reloading module: Hello`
   - `[ZScripting] module unloaded: Hello`
   - `[ZScripting] module loaded: Hello (from I:/ZEngineDemo/...)`
   编辑器全程 60 FPS 不卡顿，没有任何观察者订阅时的 `NotifyModuleReloaded` 路径也不崩（空 `vector` 的 snapshot 直接结束循环）。
4. **Project 窗口双击**：`.ts/.tsx/.js/.json` 走 `openInExternalEditor`；`code` 在 PATH 上时（VSCode 安装器默认会加）会直接拉起 VSCode 打开该文件，否则回退到系统默认编辑器。注：交互验证留给用户（Headless 自动化脚本不易测 GUI）。



### Phase 7 — `m_serialized_fields` 持久化字段覆写 ✅

**目标**：把 P5 推迟掉的 "Inspector 编辑 TS 类的 public 字段，覆盖 `class Foo { speed = 1.5 }` 的初始化器" 这件事真正打通。等价于 Unity `[SerializeField]`：值在 `.scene` / 预制中持久化，跨 hot-reload 保留，跨进程重启保留。

**最终落点（与原草稿的差异）**：

1. **存储不是 `std::map<string,string>`，而是 `std::vector<eastl::string>` 平铺成 `[k0,v0,k1,v1,...]`**。原因：`SerializeTraits` 只对 `eastl::string` 有特化、对 `std::vector<T>` 有泛型路径，但**没有** `pair<eastl::string,eastl::string>` 路径（`SerializeTraits.h` 只把 pair 与 std::string 连起来）。平铺向量是唯一不需要新增 traits 就能 round-trip 的形态，且每个 entry 占一行 YAML，diff/3-way merge 友好。配套 `GetSerializedField/SetSerializedField/RemoveSerializedField/GetSerializedFieldCount/GetSerializedFieldsRaw` 维护偶数长度不变量。

2. **C++ 不解析值的类型**。所有覆写值都是字符串，类型解释发生在 JS 侧的 `__zApplyField(instance, key, valueStr)` shim 里：读 `typeof instance[key]`，按 `number → parseFloat` / `boolean → "true"||"1"` / `string → 原样` 强转。这套规则的好处是引擎不需要单独维护"反射 schema"——TS 类的 field initializer 自己就是 schema 的来源（`class Foo { speed = 1.5 }` 让 typeof 在 OnAwake 之前就是 `"number"`）。

3. **应用时机：`CreateInstance` 之后、`OnAwake` 之前**。`TypeScriptComponent::BindAndAwake` 在 `m_js_instance = instance;` 之后立即调 `ApplySerializedFields()`，再 `InvokeInstanceMethod(instance, "OnAwake")`。这保证：
   - 首次绑定（场景加载后的 `postLoadResource`）：磁盘上的覆写在 OnAwake 看到的就是已生效状态。
   - 热重载 rebuild（P6 路径）：observer 触发的 `BindAndAwake()` 同样会再走一遍 `ApplySerializedFields()`，**Inspector 里编辑过的值不会被一次保存还活着的 `.ts` 改动冲掉**。这是和 P6 "stateless rebuild" 的有意区别：脚本作者状态（authoring data）保留，gameplay 状态（runtime state）依然丢——和 Unity Editor 一致。
   - Inspector 实时编辑：`LiveSetField` 同时写入 `m_serialized_fields` 与 live JS 实例，所以下一次 reload 也走相同的 apply 路径。

4. **Inspector 字段发现机制**：JS 侧 `__zEnumerateFields(instance)` 走 `Object.keys(instance)`（只列**自有可枚举属性**——也就是 field-initializer 设置过的那些；prototype 上的方法天然被滤掉）。返回的是用 `\u0001` / `\u0002` 分隔的扁平字符串：每条 record 是 `name\u0001type\u0001currentValue`，records 之间用 `\u0002` 隔开。选用控制字符是因为 pesapi 的 `pesapi_get_value_string_utf8` FFI 比传一个 array of objects 简单太多，且这两个字节绝对不会出现在合法 JS 标识符 / typeof 字符串里。C++ 侧 `EnumerateInstanceFields` split 出 `(name, type, default)` 三元组返回给 Inspector。

5. **过滤规则**：`__zEnumerateFields` 跳过 `typeof === 'function'`（成员方法）、跳过 `name[0] === '_'`（约定的 private）、跳过 `name[0] === '$'`（外部库常用的 "engine internal" 命名）。这套规则匹配 Unity 默认序列化器对 `private` 字段的处理（除非你显式 `[SerializeField]`，否则不出现在 Inspector）；`P7Smoke.ts` 里的 `_ticks` 走的就是这条路。

6. **未编辑器的 `Object`/`Array` 字段**：`__zApplyField` 对非标量 typeof 直接返回 `false`，C++ 侧记 WARNING 但不阻塞其他字段；Inspector UI 显示为只读 `(object) [object Object]` 之类的灰字。等真正需要时再补：要么扩展为 JSON 序列化往返，要么走 Unity 的 `ScriptableObject` 子资产路线。这条边界在风险表里有标。

7. **Inspector 端 widgets**：`DrawTypeScriptComponentScriptFields` 在 `inspector_window.cpp` 内部以 free function 暴露，组件遍历循环里 `dynamic_cast<TypeScriptComponent*>` 命中时调一次。三种 widget：`InputDouble`（覆盖 int+float，JS 不区分）/`Checkbox`/`InputText`（256 字节缓冲）。仅 `EnterReturnsTrue` 时回写到 `LiveSetField`，避免每帧因焦点漂移而被覆盖。组件未绑定（编辑器刚打开、模块尚未加载）时退化为只读列出 `m_serialized_fields_raw`，让用户至少能看见磁盘上的覆写。

#### P7 验收（实测）

1. **Build**：增量 Debug，ZRuntime + ZEditor 0 错误。新增/修改：
   - `TypeScriptComponent.{h,cpp}`：`m_serialized_fields` 字段、6 个访问器、`ApplySerializedFields/LiveSetField/EnumerateLiveFields`、`BindAndAwake` 钩子。
   - `ScriptingManager.{h,cpp}`：薄转发 `ApplyInstanceField/EnumerateInstanceFields`。
   - `ScriptEnv.{h,cpp}`：实际 pesapi 调用 + `__zApplyField`/`__zEnumerateFields` 注入到 globalThis（紧挨着 P5 的 `__zNewInstance`）。
   - `inspector_window.cpp`：`DrawTypeScriptComponentScriptFields` 自由函数 + 组件循环里的 `dynamic_cast` hook。

2. **冒烟脚本** `I:\ZEngineDemo\Scripts\P7Smoke.ts`：声明 `speed:number=1.5; enabled:boolean=true; label:string="hi"; private _ticks=0;`。Inspector 里 "Script Fields" 应当列出 3 行（`_ticks` 被过滤），每行用对应的 widget；改 `speed=4.2`、勾掉 `enabled`、改 `label="changed"` 后 BqLog `[ZScripting]` 出现 `P7Smoke.OnUpdate speed=4.2 enabled=false label=changed`。

3. **跨重启持久化**：保存场景 → 退出 ZEditor → 重新启动 → 选中同一 GameObject。Inspector "Script Fields" 列出来的 3 行应当**直接是 `4.2 / false / "changed"`**（说明 `m_serialized_fields` 序列化到 `.scene` 并 deserialize 回来了），无需手动重新 apply。OnAwake 日志同样反映这些值。

4. **跨 hot-reload 持久化**：在 (3) 之上再随便保存一次 `P7Smoke.ts`（例如改个注释）。BqLog 内出现 P6 的标准 `hot-reloading module: P7Smoke` 三连，紧接着 `OnAwake` 的日志显示**仍是 `4.2 / false / "changed"`**——说明 reload 后的 fresh instance 在 OnAwake 之前重放了 `m_serialized_fields`。这是和 P6 stateless rebuild 的关键区别。

5. **不会泄漏到 prototype 方法**：`P7Smoke` 上 `OnAwake/OnUpdate` 是 prototype 方法，`Object.keys(instance)` 不会列出来；Inspector "Script Fields" 不出现 OnAwake/OnUpdate 行。

6. **未绑定回退**：在 `m_script_guid` 留空 / 模块还在编译时打开 Inspector，Script Fields 区显示 "(component not bound; live editing disabled)" 加任何已存在的 raw 覆写。无崩溃。


---

## 5. 跨边界调用规范（puerts 绑定）

为了让 TS 能调引擎 API（`new GameObject`、`transform.position` 等），需要把 ZEngine 的反射表注入 v8/QuickJS。

**短期**（Phase 4 内）：手写 5–10 个常用类型的绑定（GameObject、TransformComponent、Vector3、Quaternion、Time、Input、Debug.Log）。

**长期**（不在本设计单内）：写一个 `BindingGenerator` 扫所有 `REGISTER_CLASS(...)` + Transfer 字段，自动生成 `Intermediate/Typings/zengine.d.ts` + C++ 绑定 cpp。这就是 puerts 自带 `Puerts_il2cpp.cpp.txt` 在做的事情，留作 future work。

---

## 6. 风险与规避

| 风险 | 规避 |
|---|---|
| 用户没装 node/tsc | Phase 3 降级模式 + Editor 顶部黄条提示 |
| TS 与 C++ 反射对不齐（命名/可见性差异） | Phase 7 已落地 `m_serialized_fields` 字符串覆写（typeof-based coercion），避免锁死 ABI；后续 BindingGenerator 来补强类型 |
| Hot Reload 时 instance state 丢失 | 参考 Unity：reload 后重新 OnAwake，**不**保留旧字段值（用户预期一致） |
| Editor 和 Runtime 的 puerts JsEnv 不能共享对象 | 二者本来就是不同进程；只共享 `.js` 文件 + `.zasset` 数据 |
| `tsc --watch` 子进程僵尸残留 | `TypeScriptCompiler::Shutdown` 用 `TerminateProcess`/`kill`，注册到引擎 atexit |
| Build Player 时怎么打包 .js？| Player 构建管线里把 `Intermediate/Scripts/` 复制到 player package；本设计单不展开 |

---

## 7. 落地顺序与可验证里程碑

| 阶段 | 验证标准 | 预计代码量 |
|---|---|---|
| P1 ProjectInfo | 启动 `I:\ZEngineDemo`，自动出现 `Assets/Scripts/`、`Intermediate/`、`tsconfig.json`、`package.json` | ~80 行 |
| P2 ScriptAsset | Project 窗口能看到 `.ts` 节点，右侧 Inspector 显示 ScriptAsset 元数据 | ~250 行 |
| P3 TypeScriptCompiler | 改 `.ts` 后 `Intermediate/Scripts/*.js` 自动更新，控制台可见 tsc 输出 | ~300 行 |
| P4 ScriptingEngine | C++ 调用 `LoadModule("PlayerController")` 后，`Debug.Log("hi")` 在控制台输出 | ~400 行 |
| P5 TypeScriptComponent | 把 `PlayerController.ts` 拖到 GameObject 上，Play 时 `OnUpdate` 被调用 | ~350 行 |
| P6 Hot Reload | Play 期间改 `OnUpdate` 输出文本，保存后 5s 内场景里立即看到新输出 | ~150 行 |

**总计 ~1500 行新代码 + 少量改动现有文件**。每阶段独立编译/可演示。

---

## 8. 与现有系统的对接清单

需要改动的现有文件（不删不重构，只增量）：

| 文件 | 改动 |
|---|---|
| `engine/Source/Runtime/project/ProjectInfo.{h,cpp}` | +scripts_dir / intermediate_dir 字段 + 三个 getter + ensureScriptsScaffold |
| `engine/Source/Runtime/Resource/Asset/AssetManager.cpp` | RegisterExtension(".ts", ScriptAssetImporter) |
| `engine/Source/Editor/editor_window/project_window/project_window.cpp` | 双击 .ts → 调 OpenInExternalEditor；显示 .ts/.js icon |
| `engine/Source/Editor/editor_window/inspector_window/*` | 渲染 TypeScriptComponent UI |
| `engine/Source/Editor/menu/components_menu.{h,cpp}` | "Add Component → Scripts → TypeScript Behaviour" 菜单项 |
| `engine/Source/Editor/CMakeLists.txt` | 已 GLOB_RECURSE，新建子目录自动收 |
| `engine/Source/Runtime/CMakeLists.txt` | 同上 |

需要新建的文件（10 个左右）：

```
engine/Source/Runtime/Resource/Script/ScriptAsset.{h,cpp}
engine/Source/Runtime/Scripting/ScriptingEngine.{h,cpp}
engine/Source/Runtime/Scripting/JSObjectHandle.{h,cpp}
engine/Source/Runtime/Function/Framework/Component/Script/TypeScriptComponent.{h,cpp}
engine/Source/Editor/Scripting/TypeScriptCompiler.{h,cpp}
engine/Source/Editor/Scripting/ScriptScaffoldTemplates.h     # tsconfig.json/package.json 模板字符串
```

---

## 9. 一个最小工作示例（验收用）

`I:\ZEngineDemo\Scripts\Hello.ts`：
```ts
import { Behaviour, Debug } from "zengine";

export class Hello extends Behaviour {
    private elapsed = 0;
    OnUpdate(dt: number): void {
        this.elapsed += dt;
        if (this.elapsed > 1.0) {
            Debug.Log(`Hello from TS, gameObject = ${this.gameObject.name}`);
            this.elapsed = 0;
        }
    }
}
```

操作流程：
1. 用 Launcher 打开 `I:\ZEngineDemo\ZEngineDemo.zproject` → ZEditor 启动
2. ZEditor 启动时自动建 `Scripts/`、`Intermediate/`、`tsconfig.json`、`package.json`，并启 `tsc --watch`
3. 在 Project 窗口右键 → Create → TypeScript Script → 命名 `Hello`，引擎用模板生成上面的 `Hello.ts`
4. tsc watch 自动 emit `Intermediate/Scripts/Hello.js`，ScriptingEngine 加载
5. Hierarchy 里随便建个 GameObject，Inspector → Add Component → TypeScript Behaviour，把 `Hello` 拖到 Script 槽位
6. 按 Play，控制台每秒打印 `Hello from TS, gameObject = ...`
7. 不停 Play，编辑 Hello.ts 把 `> 1.0` 改成 `> 0.5`，Ctrl+S → 控制台立刻每 0.5s 打印一次

构建 Player.exe 后，把 `I:\ZEngineDemo` 跟 Player.exe 一起拷贝到另一台机器（含 `Intermediate/Scripts/*.js`），运行 → 同样行为。**Editor 和 Runtime 加载的是同一份 .js**。

---

## 10. 不在本设计内（明确划界）

- TS → C++ 反射自动绑定生成器（手写绑定足够 demo）
- 调试器（vscode-attach to puerts）
- AOT / NativeAOT（QuickJS 解释执行已够 gameplay）
- 多 JsEnv / 沙盒
- `.tsx` / React-like UI 框架
- TS Reload 时跨 reload 保留字段值（Unity 也不保留）

这些可以作为后续单独 RFC，不阻塞本设计落地。
