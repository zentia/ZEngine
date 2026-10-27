# TypeScriptComponent 真身：把 tsc 产物挂到 GameObject 上跑完整生命周期

## 1. 为什么单独写这个测试

在它之前，脚本侧已经有两个测试：

| 测试 | 钉住的是 |
|------|----------|
| `ScriptVMSmokeTest` | 手写 CommonJS 能被 `ScriptEnv` 加载、实例能跑 |
| `ScriptTsChainSmokeTest` | **tsc** 的产物能被 `ScriptEnv` 加载、原型链对 |

但它们有一个共同点：**都是自己 new 一个 `ScriptEnv` 手喂模块**。用户真正走的路是：

```
<project>/Scripts/Foo.ts
  -> tsc -> <project>/Intermediate/Scripts/Foo.js
  -> ScriptRegistry 扫到它，给一个确定性 GUID
  -> 用户把脚本拖到 Inspector / 组件上记下这个 GUID
  -> TypeScriptComponent::PostLoadResource
       -> ResolveScript: guid -> ScriptAsset -> module id + 类名
       -> LoadModule -> CreateInstance
       -> 注入 gameObject / transform / component
       -> OnAwake / OnStart
  -> 每帧 Tick 转发 OnUpdate
  -> 保存 .ts 后热重载：OnDestroy -> 重建实例
  -> 组件销毁：OnDestroy
```

这条路上任何一环断掉（guid 查不到、类名解不出来、module id 拼错、注入时机晚了、
热重载没重新实例化），前面两个测试**全都不会红**。

`ScriptComponentSmokeTest` 补的就是这段：47 项检查，A–G 七组。

## 2. 测的是什么

| 组 | 内容 |
|----|------|
| A0 | 类型表是齐的（`GameObject` / `Transform` / `TypeScriptComponent` 都注册了，且有 factory） |
| A | `ScriptRegistry` 扫描：`.ts` 被扫到、guid 是 32 位 hex、默认类名解得出、编译产物路径配对 |
| B | 挂载：`AddComponentByTypeName` 造出真组件 -> `SetScriptGuid` -> 实例建起来，`gameObject` / `transform` / `component` 三个宿主对象都注入成功 |
| C | 每帧 `Tick(0.5f)` 转发到 `OnUpdate`，值和帧数都对 |
| D | Inspector 字段覆盖（`LiveSetField`）立刻生效 |
| E | 热重载：`OnDestroy` 被调、实例重建、**游戏状态归零**、**Inspector 覆盖保留** |
| F | 类名解析兜底：`export { X }` 写法解不出默认类名时，靠组件上的 `m_ClassName` 兜住 |
| G | 销毁：解绑 / 析构各触发一次 `OnDestroy`，且不会触发第二次 |

## 3. 热重载的语义（E 组是重点）

热重载是**无状态重建**：丢掉 JS 实例 -> `OnDestroy` -> 重新 `OnAwake` / `OnStart`。

| 数据 | 跨重载 |
|------|--------|
| 游戏状态（`updateCount` / `originX`） | **不保留**（E6 / E7） |
| Inspector 改过的字段（`speed=10`） | **保留**（E8） |

理由：`m_SerializedFields` 存的是 authoring data，不是游戏状态。用户在 Inspector
里改的值不该因为保存了一次 `.ts` 就丢。需要跨重载存活的运行时状态得自己外置
（比如写进一个 C++ 组件字段，或者像这里一样写 `globalThis`）。

顺带一个测试设计上的点：`HostedMover.OnDestroy` 把计数写进
`globalThis.__hostedDestroyCount` 而不是实例字段 —— 因为重载会丢掉整个实例，
`ReloadModule` 也会重置模块级变量，只有 `globalThis` 跨得过重编译。

## 4. 本机两个必须知道的坑

### 4.1 `TypeManager.cpp` 必须显式编进来

`AddComponentByTypeName` 和 `GetTransform()` 都走 `TypeManager` 的类型表，而类型表
是 `IMPLEMENT_REGISTER_CLASS` 的静态注册器填的，注册器最终调到
`TypeManager::GlobalRegisterType(desc)`。

本机把 `bin/libZRuntime.a` 和现编的 `.gen.cpp` 混链，**`TypeRegistrationDesc` 的布局
在 `.a` 里那份 `TypeManager.cpp` 和当前源码之间漂移了**，于是
`RegisterType` 读到的 `desc.type` 是错位后的 `nullptr`，直接 return：

```
TypeOf<GameObject>          name=[UNREGISTERED] idx=2147483648
byName GameObject=0x0 Transform=0x0 TypeScriptComponent=0x0
GetComponentByTypeName(Transform)=0x0  GetTransform()=0x0
```

把 `TypeManager.cpp` 也列进 `--extra-source`，让 `GlobalRegisterType` 和 desc 的构造
方用同一份布局，就好了：

```
TypeOf<GameObject>          name=GameObject idx=1
TypeOf<Transform>           name=Transform  idx=5
TypeOf<TypeScriptComponent> name=TypeScriptComponent idx=3
GetComponentByTypeName(Transform)=0x107b5dcc0  GetTransform()=0x107b5dcc0
```

**这是本机混链的产物，不是引擎的 bug** —— 完整 cmake 构建里两边同一份源码。
测试里 A0 组就是用来把这个前提钉住的：忘了带 `TypeManager.cpp`，A0 立刻红，
而不是等到 B 组才看到一个含义不明的 `nullptr`。

### 4.2 `Behaviour.gen.cpp` 要带，`Component.gen.cpp` 反而先别带

`TypeScriptComponent` 的继承链是 `TypeScriptComponent -> Behaviour -> Component -> Object`。
`AddComponentByTypeName` 内部有一步 `Cast<Component>(raw)`，走的是
`Type::IsBaseOf` 的范围测试 —— 那个范围是 `TypeManager::InitializeAllTypes()` 里
从 `base` 指针一路 DFS 算出来的。

* 只带 `TypeScriptComponent.gen.cpp`：`Cast<Component>` 拿不到（Component 的
  `descendantCount` 还是 0），`AddComponentByTypeName` 返回 `nullptr`。
* 带上 `Behaviour.gen.cpp`（**直接基类**）：Builder 顺着 `Behaviour -> Component`
  走上去，Component 拿到正确的范围，`Cast<Component>` 通过。
* 再额外带上 `Component.gen.cpp`：本机在 `main()` 之前就 SIGSEGV（启动期静态
  初始化阶段）。没查到底，本机先不带 —— `Behaviour` 已经足够，多带没收益。

### 4.3 分配器要配对

| 谁造的 | 用什么释放 |
|--------|-----------|
| `ObjectManager::Produce`（`AddComponentByTypeName` 走这条，普通 `new`） | `ObjectManager::Destroy`（内部也是 `delete`） |
| `ObjectManager::NewObject<T>()`（mimalloc） | `MemoryManager::DestroyObject` |

串了就在 `mi_usable_size` 里炸。测试里 GameObject 和组件统一走 `Produce` + `Destroy`。

另外销毁顺序是**先宿主后组件**：`~GameObject` 不拥有组件（只清列表），
组件先没了会在宿主的组件表里留下已释放的指针。

## 5. 怎么跑

有 cmake：

```bash
cmake -B build -DZENGINE_BUILD_SCRIPT_VM_SMOKE_TEST=ON
cmake --build build --target ScriptComponentSmokeTest
ctest --test-dir build -R ScriptComponentSmokeTest
```

没 cmake（本机现状）—— 一般不用手敲，`scripts/check_ts_chain.py` 自带完整清单：

```bash
python3 scripts/check_ts_chain.py
```

它跑 5 步：导出当前 d.ts -> tsc 编译 -> 与签入的 `TscOutput/` diff -> 跑
`ScriptTsChainSmokeTest` -> 跑 `ScriptComponentSmokeTest`。

单独跑组件测试：

```bash
python3 scripts/run_smoke_test.py \
  engine/Source/Runtime/Scripting/ScriptVM/Test/ScriptComponentSmokeTest.cpp \
  --template Function/Audio/AudioSystem.cpp \
  --extra-source engine/Source/Runtime/Scripting/Backends/QuickJS/BackendQuickJS.cpp \
  --extra-source build/Generated/ZHT/ZRuntime/Src/ScriptMath.gen.cpp \
  --extra-source build/Generated/ZHT/ZRuntime/Src/ScriptTime.gen.cpp \
  --extra-source build/Generated/ZHT/ZRuntime/Src/ScriptInput.gen.cpp \
  --extra-source build/Generated/ZHT/ZRuntime/Src/KeyCodes.gen.cpp \
  --extra-source build/Generated/ZHT/ZRuntime/Src/GameObject.gen.cpp \
  --extra-source build/Generated/ZHT/ZRuntime/Src/Transform.gen.cpp \
  --extra-source build/Generated/ZHT/ZRuntime/Src/Behaviour.gen.cpp \
  --extra-source build/Generated/ZHT/ZRuntime/Src/TypeScriptComponent.gen.cpp \
  --extra-source engine/Source/Runtime/Core/Time/TimeState.cpp \
  --extra-source engine/Source/Runtime/BaseClasses/GameObject.cpp \
  --extra-source engine/Source/Runtime/BaseClasses/TypeManager.cpp \
  --extra-source engine/Source/Runtime/Scripting/Wrapper/ScriptEnv.cpp \
  --extra-source engine/Source/Runtime/Function/Framework/Component/Script/TypeScriptComponent.cpp \
  --run-arg=--resource-dir \
  --run-arg=engine/Source/Runtime/Scripting/ScriptVM/Test
```

最后两份（`TypeManager.cpp`、`TypeScriptComponent.cpp`）不是"因为在 `.a` 里太旧"，
而是**被测代码本身**：类型表的构造和组件的生命周期就住在那两个文件里，测它们
就必须编它们（否则改了源码测试不会红 —— 这一点在做负向验证时踩到了，见第 6 节）。

## 6. 负向验证（证明这些断言真的会红）

三处注入，都在 `TypeScriptComponent.cpp` 里，都已还原：

| 注入 | 结果 |
|------|------|
| 把 `kTransform` 的注入挪到 `OnAwake` **之后** | B10 失败（1 项：`got='false' want='true'`） |
| `OnModuleReloaded` 里注释掉重建的 `BindAndAwake()` | 7 项失败：E3 / E6 / E7 / E9，以及 G1 / G3 / G4（计数跟着错位） |
| `BindAndAwake` 里注释掉 `ApplySerializedFields()` | E8 / E9 失败（`speed` 退回类默认的 2） |

第二行里 E5（`awakeCount` 回 1）**没有**红 —— 因为不重建的话 `awakeCount` 本来
就还是 1。这说明 E5 单独看不够，必须和 E6 / E7（游戏状态归零）合起来才钉得住
"确实是新实例"。

还原后复跑 **47/0**。

## 7. 刻意没做 / 顺带观察到的事

* **`CommandSystem` 是手填的**：`CommandSystem::Initialize()` 从 argv 解析
  `-p/--project`，测试进程没这个参数，会把 `project_path` 冲成空，于是
  `ProjectInfo::LoadFromFile` 失败、`InitializeAll` 整轮回滚。测试里手填
  `GetProjectPath()` / `GetWorkingDir()` 再 `SetInitialized(true)` 跳过它。
* **`~GameObject` 不销毁组件**：`GameObject::~GameObject` 里那个 `for` 循环是
  空的（只 `m_Components.clear()`）。所以销毁 GameObject 不会带走它的组件。
  这看起来像是没写完，但组件的所有权可能归场景/资源系统，不在 GameObject 手上
  —— 没改，只在这里记一笔，测试按"宿主不拥有组件"来写。
* **V8 后端**：和另外两个脚本测试一样，非 QuickJS 构建直接 `#error`。
* **Prefab / 场景反序列化过来的组件**：本测试测的是 `AddComponentByTypeName` 这条
  运行时挂载路径；从资产文件里反序列化出来的 `TypeScriptComponent` 是另一条路，
  **由 `ScriptComponentPrefabTest` 单独覆盖（见第 8 节）**。资产分两轨
  （`.object.json` 早已废弃）：
  * **编辑器：`.prefab`** —— 人类可读的 YAML 对象图，扩展名即权威（不用读
    header），由 `AssetManager::WriteObjectsToYaml` 写。
  * **真机 / legacy：`.zasset`** —— 二进制 SerializedFile（`AssetFileHeader`
    176 字节，由 `SerializedFile::WriteHeaderAndMetadata` 写）。它是所有 ZEngine
    资产共用的统一容器，类比 UE 的 `.uasset`；由
    `AssetManager::WriteObjectsToDiskThreadSafe` 写。
  * 两条路**共用同一份有序对象表和同一套引用模型**：文件内引用用 `ImmediatePtr`
    （file-local fileID + pathID），跨文件引用用 `PPtr`（GUID）。判定时
    `path.extension() == ".prefab"` 优先，否则走 `isPrefabZAsset()`（legacy 二进制）。
* **那条路验到哪一步**：`m_script_guid` / `m_class_name` /
  `m_serialized_fields` 三个字段都是 `ZPROPERTY` 且都在 `-- Serialised --` 段内，
  会随 prefab / zasset 一起 round-trip；反序列化后走
  `LoadPrefabAsset → Instantiate → CloneSubtree → PrefabPostLoadDriver::Run`，
  最终也落到同一个 `PostLoadResource` 入口。第 8 节那个测试把这三件事都钉住了：
  还原出来的 guid 能绑上、override 跟着还原、并在 `ApplySerializedFields()` 里
  真的生效。
* **顺带观察到的一个层级限制**：`PrefabPostLoadDriver` 靠
  `{child Transform → owning GameObject}` 映射做 DFS 下发 `PostLoadResource`。
  `.prefab` 路径能拿到完整的 `known_gameobjects`（YAML reader 直接给出），多层层级
  全通；legacy `.zasset` 路径没有这份 flat list，只能从 root 发现，多层层级仍受
  Phase-1 限制 —— 挂在下层子节点上的 `TypeScriptComponent` 在二进制路径上有可能
  拿不到 `PostLoadResource`。（目前没测：本测试的宿主是单层根节点。）

## 8. prefab / zasset 反序列化：`ScriptComponentPrefabTest`

第 7 节留下的那条空白由这个测试补上（63 项）。它做的是**真存盘再真读回来**：

```
PrefabUtility::SaveAsPrefabAsset(go, *.prefab)      # YAML 对象图
PrefabUtility::SaveAsPrefabAsset(go, *.zasset)      # 二进制 SerializedFile
  -> PrefabUtility::InstantiateFromPath(path, invoke_post_load)
     -> LoadPrefabAsset      整图读回，读一次后缓存（path 派生的 InstanceID）
     -> Instantiate          CloneSubtree 深克隆（模板不动）
     -> PrefabPostLoadDriver::Run  沿 Transform.m_Children DFS 逐组件下发
                                   PostLoadResource
```

### 8.1 分组

| 组 | 钉什么 |
|----|--------|
| A0 | 类型表前提（GameObject / Transform / TypeScriptComponent / **PrefabAsset**） |
| A | ScriptRegistry 扫到 `HostedMover.ts`，拿到 guid |
| B | 造宿主：`Produce(GameObject)` + Transform + TypeScriptComponent，设 guid，记 `speed=7` 的 Inspector override，**两份资产都写盘成功** |
| C | `.prefab` 读回：克隆是新对象、名字还原、guid 还原、**凭 guid 绑上**、override 还原、override 生效、Awake 一次、宿主注入、一帧走 3.5 |
| D | `.zasset` 读回：同上 13 条 |
| E | `invoke_post_load=false`：树照常克隆、guid 照常还原，**但不绑** |
| F | 两轨一致性：guid / override / 活实例 speed 都相同 |
| G | 空白 prefab：只有 Transform 的根也能存回来（守 Content Browser 新建资产的产出，见 8.6） |

### 8.2 为什么 override 用 `speed=7`

`HostedMover` 的类默认 `speed` 是 2。反序列化出来的实例一帧走 **3.5**
（=7×0.5）而不是 1（=2×0.5），才说明 override 真的穿过了磁盘并在
`ApplySerializedFields()` 里生效。只看"字段在不在"不够，得看它有没有起作用。

对照组也钉住了：内存里那份用 `SetSerializedField`（只写 authoring 数据，不推活
实例），所以它的活实例仍是 2（B5）。克隆体上看到 7，就一定是文件里带过来的。

### 8.3 本机跑（无 cmake）

`scripts/check_ts_chain.py` 的第 6 步自带完整清单。要点是**除了**第 5 节那套，
还要多现编 7 份、并踢掉 5 个 .a 成员：

```bash
--extra-source engine/Source/Runtime/Resource/Prefab/PrefabUtility.cpp
--extra-source engine/Source/Runtime/Resource/Prefab/PrefabSubtreeCloner.cpp
--extra-source engine/Source/Runtime/Core/YamlSerialize/YamlObjectGraph.cpp
--extra-source engine/Source/Runtime/Resource/Asset/AssetManager.cpp
--extra-source engine/Source/Runtime/BaseClasses/ImmediatePtr.cpp
--extra-source engine/Source/Runtime/BaseClasses/IPPtrResolver.cpp
--extra-source .../Component/Transform/Transform.cpp
--exclude-archive-member YamlObjectGraph.cpp.o AssetManager.cpp.o \
--exclude-archive-member ImmediatePtr.cpp.o IPPtrResolver.cpp.o Transform.cpp.o
```

两个坑，都是同一类病（`.a` 比源码旧）：

* **`PrefabUtility` 里根本没有 `LoadPrefabAsset`** —— 整图加载是 .a 构建之后加
  的。不现编的话符号是 `dynamically looked up`，一跳进去就崩。
* **构造方和生成式 `Transfer()` 不是同一份布局** —— `Transform` 按旧布局 `new`
  出来、又被按新布局序列化，`CloneSubtree` 里 `CloneObjectViaSerialization` 读
  回来就是越界（`si_addr` 是个小负数，崩在 `CachedReader::UpdateReadCache`）。
  这是最隐蔽的一条：写盘那一侧完全正常，文件的内容逐字节都对。

现编了同名 .cpp 之后，链接期两边都定义同一批符号就是 duplicate symbol —— 因为
那个旧成员仍会因为别处引用了它独有的旧符号被拉进来。所以加了
`--exclude-archive-member`：它复制一份 `.a` 到 `/tmp` 再 `ar -d`（按成员列表缓
存，只付一次复制的代价）。完整 cmake 构建里所有 TU 同源码，这些一条都不需要。

### 8.4 负向验证（都还原后复跑 63/0）

| 注入 | 结果 |
|------|------|
| `BindAndAwake` 里注释掉 `ApplySerializedFields()` | C10/C13 + D10/D13 红（活实例 speed 退回类默认 2，一帧走 1.0）。**C7/C8/C9 保持绿** —— 说明"override 存下来了"和"override 生效了"是两条独立的断言 |
| `PrefabUtility::Instantiate` 里把 `invoke_post_load` 强制成 false | C8/D8 红（checks 少 8：不绑时 C10–C13 + D10–D13 直接跳过，63 → 55）。**C7（guid 还原）保持绿** —— 说明"还原"和"绑"是两件事 |

### 8.5 刻意没做

* **多层层级**：宿主是单层根节点。`PrefabPostLoadDriver` 在 legacy 二进制路径上
  没有 flat GameObject list（见第 7 节），挂在下层子节点上的组件能不能拿到
  `PostLoadResource` 仍旧没测 —— 这是两轨之间一个已知的行为差异。
* **清理**：测试刻意不销毁克隆出来的对象。`PrefabSubtreeCloner` 的分配器不是
  `ObjectManager::Destroy`（`= delete`）那一套，混着收会在 `mi_usable_size` 里
  SIGSEGV（第 4.3 节踩过一次）。测试进程退出即回收。

### 8.6 G 组：空白 prefab 与 Content Browser 的新建资产

Content Browser 的 `Create ▸ Prefab`（旧标签 "Object"）产出的就是一个**只带
Transform 的空 GameObject** 的 `.prefab`。G 组用与编辑器**完全相同**的配方
（`Produce(GameObject)` → `AddComponentByTypeName("Transform")` →
`SyncSerializedComponents()` → `SaveAsPrefabAsset`）跑一遍存回，守住这条路径。

为什么必须有一个根 GameObject、不能像 `.scene` 那样只写一个空头：

```
PrefabUtility::LoadPrefabAsset
    if (asset->GetRootGameObject() == nullptr)   // PrefabUtility.cpp:454
        -> LOG_ERROR("PrefabAsset has no root GameObject"); return nullptr;
```

`LevelRes` 头可以空着（场景里的 GameObject 是保存时才追加的），`PrefabAsset`
不行 —— 只写头的 `.prefab` 是一个**打不开的坏资产**。G5 就是为了挡住这个回归。
