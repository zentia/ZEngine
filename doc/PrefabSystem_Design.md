# ZEngine Prefab System — Design & Implementation RFC

> Status: Draft v1（基于 Unity 2023.1 调研 + ZEngine 现状事实核对）
> Scope: 完整版（标准 + 嵌套 Prefab + Prefab Variant + Unpack + Merge）
> Asset 部分：Runtime（`engine/Source/Runtime/Resource/Prefab/`）
> Editor 部分：Editor（`engine/Source/Editor/Prefab/`）
> 文件格式：`.zasset`（二进制 SerializedFile，ZEngine 所有资产共享的统一扩展名，参考 UE `.uasset` 单容器约定）

---

## 0. 事实基线（Why we can map Unity 1:1）

ZEngine 已有的 Unity-equivalent 设施：

| 概念 | ZEngine | Unity |
| --- | --- | --- |
| 通用对象基类 | `class Object` (BaseClasses/Object.h) | `Object` |
| 反射 / 类型注册 | `Type` + `TypeManager` + `REGISTER_CLASS` | `RTTI` + `Object` |
| 跨文件指针 | `PPtr<T>`（InstanceID + 持久化加载） | `PPtr<T>` |
| 文件内指针 | `ImmediatePtr<T>`（FileID + PathID） | `ImmediatePtr<T>` |
| GameObject | `BaseClasses/GameObject.h`（持有 `vector<ImmediatePtr<Component>>`） | `GameObject` |
| Component | `Function/framework/component/Component.h` | `Component` |
| Transform | `Function/.../transform/transform_component.h`（**当前没有 parent/children**） | `Transform`（含层级） |
| 序列化 | `SerializedFile` + `Transfer<T>` 双向 | `SerializedFile` + `Transfer` |
| 资产容器 | `.zasset` 二进制 | `.asset` |
| ID 分配 | `ObjectIDAllocator` (GObjectID), `ObjectManager` (InstanceID) | 等价 |

**结论**：Prefab 数据模型可以**直接套用 Unity 的 PrefabInstance/PropertyModification 模型**；唯一需要先补的前置依赖是 **Transform 父子关系**（Phase 0）。

---

## 1. 数据模型

### 1.1 Runtime 部分（任何运行时都需要的最小结构）

```
                    .zasset (on disk, binary SerializedFile)
                              │
           ┌──────────────────┴──────────────────┐
           │              PrefabAsset            │
           ├──────────────────────────────────────┤
           │ rootGameObject : ImmediatePtr<GO>    │
           │ allObjects     : vector<ImmediatePtr<Object>>  // GO + Component 全列表（用于按 fileID 索引）
           │ schemaVersion  : uint32               // 升级用
           └──────────────────────────────────────┘
```

> 跨 `.zasset` 引用走 `PPtr`（路径 + `localIdentifierInFile`），无需在 `PrefabAsset` 上保留独立 GUID 字段——参考 UE：UE 没有 `.meta` 侧车文件，跨包引用用 `FSoftObjectPath`，`FPackageFileSummary::PackageGuid` 仅作为内容指纹给 cooker/源代码控制使用，不参与解析；ZEngine 当前完全没有 `.meta` 文件机制，AssetManager 通过 `GetInstanceIDFromPathAndFileID()` 解析 PPtr，与 GUID 无关。


- `PrefabAsset` **本身是一个 `Object`**，注册到反射系统。它和它内部所有 GameObject/Component 共享同一个 `SerializedFile`，每个对象有 `localIdentifierInFile`（即 fileID）。
- 加载时：通过 `AssetManager::ReadObject<PrefabAsset>(path)` 读出整棵树；内部 GO/Component 由 `ImmediatePtr` 自动解析为本进程的 `Object*` + InstanceID。
- 没有 override / nesting 概念时，`PrefabAsset` 已经能作为「场景中可用的 GameObject 模板」使用。

#### Instantiate 接口（运行时）

```cpp
// engine/Source/Runtime/Resource/Prefab/PrefabUtility.h
class PrefabUtility {
public:
    // 运行时纯实例化：深拷贝整棵 GO+Component 树，分配新 InstanceID/GObjectID。
    // 返回新的根 GameObject。是否注册到 Level 由调用方决定（Level::adoptInstantiated）。
    static GameObject* Instantiate(PrefabAsset* asset);
    static GameObject* Instantiate(PrefabAsset* asset, const Transform& worldPose, GameObject* parent = nullptr);

    // 在编辑器场景中创建带 PrefabInstance 链接的实例（保留 source ↔ instance 映射）。Editor-only。
#if WITH_EDITOR
    static GameObject* InstantiatePrefabInScene(PrefabAsset* asset, Level* level);
#endif
};
```

`Instantiate` 的语义对齐 Unity `Object.Instantiate(Prefab)` —— **运行时只产生「克隆」**，完全断开与源 Prefab 的链接。这一类 GameObject 在保存场景时应原地序列化整棵树（不引用源 .prefab）。

### 1.2 Editor 部分（PrefabInstance / PropertyModification）

```
                        Scene (in editor)
                              │
                    ┌─────────┴─────────┐
                    │  PrefabInstance   │  ← 被场景作为一个普通 Object 引用
                    ├───────────────────┤
                    │ sourcePrefab : PPtr<PrefabAsset>      // 源 .prefab（GUID 引用，可跨文件）
                    │ rootInstance : PPtr<GameObject>       // 场景中实际生成的根 GameObject
                    │ modifications: vector<PropertyModification>
                    │ addedComponents   : vector<AddedComponent>
                    │ removedComponents : vector<RemovedComponent>
                    │ addedGameObjects  : vector<AddedGameObject>
                    │ removedGameObjects: vector<RemovedGameObject>
                    │ isVariant : bool                       // 区分 Variant
                    │ variantBase : PPtr<PrefabAsset>        // 仅 Variant 使用
                    └───────────────────┘

           PropertyModification  // 「源对象的某个字段被这个实例改写了」
           ├─────────────────────────────────────
           │ target          : PPtr<Object>   // 注意：指向的是「实例化后的对象」（场景对象）
           │ correspondingSource : PPtr<Object>  // 它对应的源 Prefab 内对象（fileID 在 source 文件里）
           │ propertyPath    : eastl::string    // 反射路径，"position.x" / "m_components[2].m_color"
           │ value           : Variant          // 字符串/数字/PPtr 三态（与现有 Variant.h 对齐）
           │ objectReference : PPtr<Object>     // 当 propertyPath 是引用类型时使用
```

- 上面所有结构都是 `Object` 的子类或可被反射的 `struct`，全部走 `Transfer` 序列化。
- 场景文件 (`.scene.zasset`) 里只存：`PrefabInstance` 的 fileID + `sourcePrefab` 的 GUID + 它的 modifications。**不会**把整棵 GO 树展开存进场景文件。
- 实例的实际 GameObject 子树（`rootInstance` 指向的）也存在场景文件里，但它们的字段值在加载时**会被 modifications 覆盖**（"flatten 算法"，见 §4.1）。Unity 的做法：场景里的实例对象在保存时**已经是覆盖后的值**，所以场景加载并不需要再执行 merge——**modifications 只是 editor 元数据，用于 Apply/Revert**。这点我们对齐 Unity。

### 1.3 嵌套 Prefab & Prefab Variant

#### 嵌套（Nested Prefab）

> 一个 Prefab 在它自己的层级树里 **再放入另一个 Prefab 的实例**。

数据上等价于：`PrefabAsset` 内部某个 GameObject 子树是一个 **嵌套的 PrefabInstance**。我们在 `PrefabAsset.allObjects` 里也允许出现 `PrefabInstance` 类型的对象（这与 Unity 完全一致：Unity 在 .prefab 文件里就是有 `!u!1001 &xxx PrefabInstance` 段）。

加载时的 flatten 顺序：

```
深度优先 Flatten(prefabAssetRoot)
  for each child:
     if child is PrefabInstance:
         load child.sourcePrefab → tree
         apply child.modifications onto tree
         attach tree.root under current parent
         recurse into tree for further nested PrefabInstance
     else:
         recurse normally
```

#### 变体（Prefab Variant）

> 一个 Prefab Asset 可以「**继承**」另一个 Prefab Asset：base 中的更改自动传播；自身的 modifications 在 base 之上叠加。

实现 = `PrefabAsset` 多两个字段：

```cpp
PPtr<PrefabAsset>          variantBase;       // 非空表示这是 Variant
std::vector<PropertyModification> variantOverrides; // 相对 base 的 override
```

加载/Flatten 顺序：

1. 递归 flatten `variantBase`（直到根 base）
2. 把 `variantOverrides` 套到 flattened tree 上
3. 把自己内部增删的 GameObject/Component（addedGameObjects/removedGameObjects）应用上去
4. 得到该 PrefabAsset 的 "effective tree"

PrefabInstance 引用 Variant 时，再叠加 `PrefabInstance.modifications` —— 三层叠加顺序：**base → variant overrides → instance overrides**（与 Unity 一致）。

---

## 2. 文件格式（`.zasset` — 与所有 ZEngine 资产同扩展名）

> ZEngine 所有资产（材质、Shader、纹理、Prefab 等）都序列化为单一 `.zasset` 二进制容器，约定参考 UE 的 `.uasset`。资产具体类型由 `SerializedFile` 头部的类 ID 区分，AssetManager 通过 `getAssetTypeName()` 派发。



依然是 `SerializedFile`（二进制）。布局示例（YAML 化展示，便于理解；实际是 `Transfer` 出的二进制）：

```
%YAML-equivalent
--- !PrefabAsset &1     # PathID=1, root
schemaVersion: 1
rootGameObject: { fileID: 100 }

--- !GameObject &100
name: "Player"
m_Components:
  - { fileID: 101 }   # Transform
  - { fileID: 102 }   # MeshRenderer
  - { fileID: 200 }   # 嵌套 PrefabInstance（子）

--- !TransformComponent &101
m_position: [0,0,0]
m_rotation: [0,0,0,1]
m_scale: [1,1,1]
m_parent: { fileID: 0 }
m_children: [ {fileID: 110} ]

--- !MeshRendererComponent &102
material: { fileID: 0, guid: "abcd...", type: 2 }   # PPtr 跨文件

--- !PrefabInstance &200    # 嵌套
sourcePrefab: { fileID: 0, guid: "ee...11", type: 2 }
rootInstance: { fileID: 110 }
modifications: [...]
```

**约定的 fileID 分段**（避免和外部冲突）：

| 段 | 用途 |
| --- | --- |
| `1` | `PrefabAsset` 自身 |
| `100..` | GameObject |
| `200..` | 嵌套的 PrefabInstance 段（在 PrefabAsset 内部） |
| 其它 | Component（与对应的 GameObject 紧挨着分配） |

> 这只是建议性约定；实际可由 `SerializedFile` 自己分配，不强制。

---

## 3. 关键算法

### 3.1 Instantiate（运行时核心）

```
GameObject* Instantiate(PrefabAsset* asset)
{
    PrefabInstantiateContext ctx;     // sourceObj* → newObj* 映射

    // 1. Flatten（如果是 Variant，先把 base 链 flatten 出来；下文 §3.4）
    EffectiveTree tree = Flatten(asset);

    // 2. 深拷贝
    GameObject* newRoot = CloneObjectTree(tree.root, ctx);

    // 3. 重映射所有内部 PPtr / ImmediatePtr：源 → 新对象
    for (Object* obj : ctx.cloned)
        Remapper::RemapInternalReferences(obj, ctx);

    // 4. 注册到 ObjectManager 分配新 InstanceID
    for (Object* obj : ctx.cloned)
        ObjectManager::Instance().AllocateAndAssignInstanceID(obj);

    // 5. 调用每个 Component 的 postLoadResource(parent_go)
    InvokePostLoadRecursive(newRoot);

    return newRoot;
}
```

`CloneObjectTree`：用反射 + `MemoryManager::CreateObject<T>()` + `Transfer` 实现「序列化到内存 / 反序列化回来」的等价深拷贝。**非常重要**：先把整棵子树的所有 Object 全部 clone 出来再做引用重映射，否则会出现野指针。

### 3.2 Apply（实例 → 源 Prefab 写回）

```
PrefabUtility::ApplyPrefabInstance(PrefabInstance* instance, PrefabAsset* asset)
{
    // 1. 对每个 modification：找到 asset 内对应的源对象，按 propertyPath 反射写回
    for (auto& mod : instance->modifications):
        Object* sourceObj = mod.correspondingSource.Resolve();
        ReflectionWrite(sourceObj, mod.propertyPath, mod.value);

    // 2. addedGameObjects → 在 asset 的对应位置克隆一份过去（更新 corresponding 链）
    for (auto& add : instance->addedGameObjects):
        AppendToAsset(asset, add);

    // 3. addedComponents 同理
    // 4. removedComponents/removedGameObjects → 从 asset 中删除对应对象
    // 5. 清空已 apply 的 modifications

    AssetManager::SaveAsset(asset, asset.path);
    BroadcastPrefabChanged(asset);  // 通知所有监听这个 asset 的实例重新 flatten
}
```

### 3.3 Revert（实例 ← 源 Prefab 重新拉取）

```
PrefabUtility::RevertPrefabInstance(PrefabInstance* instance)
{
    // 把整个 instance.rootInstance 子树用 asset 重新 flatten 一次，覆盖现有对象的字段
    // modifications 全部清空
    // addedGameObjects/Components 全部移除
    EffectiveTree tree = Flatten(instance->sourcePrefab);
    OverwriteSubtree(instance->rootInstance, tree.root);
    instance->modifications.clear();
    instance->addedGameObjects.clear();
    instance->addedComponents.clear();
    instance->removedGameObjects.clear();
    instance->removedComponents.clear();
}
```

### 3.4 Flatten（Apply/Instantiate 都依赖）

```
EffectiveTree Flatten(PrefabAsset* asset)
{
    EffectiveTree tree;

    if (asset->variantBase) {
        EffectiveTree baseTree = Flatten(asset->variantBase);   // 递归
        tree = DeepCopy(baseTree);
        ApplyModifications(tree, asset->variantOverrides);
        ApplyStructuralChanges(tree, asset->addedGameObjects, asset->removedGameObjects, ...);
    } else {
        tree = DeepCopy(asset->rootGameObject);
    }

    // 递归处理嵌套的 PrefabInstance
    for (Object* obj : tree) {
        if (obj is PrefabInstance) {
            EffectiveTree child = Flatten(((PrefabInstance*)obj)->sourcePrefab);
            ApplyModifications(child, ((PrefabInstance*)obj)->modifications);
            ReplaceInTree(tree, obj, child);
        }
    }
    return tree;
}
```

### 3.5 Unpack

```
PrefabUtility::UnpackPrefabInstance(PrefabInstance* inst, UnpackMode mode)
{
    // OutermostRoot：只 unpack 最外层；保留嵌套 PrefabInstance 的链接
    // Completely  ：递归 unpack 所有嵌套
    DestroyObject(inst);   // 删除元数据，但保留 inst.rootInstance 子树
    if (mode == UnpackMode::Completely) {
        for each nested PrefabInstance under rootInstance: recurse
    }
}
```

### 3.6 Merge（Diff 算法，编辑器在每次保存/检视时计算）

> 用于「修改了实例的某个字段后，自动把这个改动登记成一条 PropertyModification」。

```
ComputeModifications(instance) -> vector<PropertyModification>
{
    EffectiveTree expected = Flatten(instance.sourcePrefab);     // 期望的"无 override"形态
    Subtree       actual   = instance.rootInstance.snapshot();
    vector<PropertyModification> mods;

    for each (sourceObj ↔ instanceObj) pair (用 corresponding 链建立) {
        for each reflected field f in sourceObj.GetType() {
            if (instanceObj.f != sourceObj.f)
                mods.append({ instanceObj, sourceObj, pathOf(f), instanceObj.f });
        }
    }
    return mods;
}
```

每次场景保存前调用一次 `ComputeModifications` 更新 `instance->modifications`。读取时反向走 §3.4。

---

## 4. 模块布局 / CMake

```
engine/Source/Runtime/Resource/Prefab/
├── PrefabAsset.h / .cpp               # Object 子类，注册反射，Transfer
├── PrefabUtility.h / .cpp             # Instantiate / Clone / Flatten 公共算法
├── PrefabInstantiateContext.h         # 克隆映射表
└── CMakeLists.txt                     # 通过现有 GLOB_RECURSE 自动收

engine/Source/Editor/Prefab/
├── PrefabInstance.h / .cpp            # Editor-only：场景中的实例
├── PropertyModification.h / .cpp      # override 元数据
├── PrefabApply.cpp / PrefabRevert.cpp # Apply/Revert
├── PrefabDiff.cpp                     # ComputeModifications
└── CMakeLists.txt                     # 加入 Editor 模块
```

> Runtime CMake 是 `GLOB_RECURSE` 风格（确认自 `engine/Source/Runtime/CMakeLists.txt`，45KB），新增源文件会被自动收录。Editor 同理。

---

## 5. 与 Transform 父子关系的关系（前置 Phase 0）

Prefab 嵌套要求 Transform 有 hierarchy。补丁要点：

```cpp
// transform_component.h（Phase 0 新增）
class TransformComponent : public Component {
    PPtr<TransformComponent>              m_parent;       // 父
    std::vector<PPtr<TransformComponent>> m_children;     // 子
public:
    void          SetParent(TransformComponent* newParent, bool worldPositionStays = true);
    TransformComponent* GetParent() const;
    size_t        GetChildCount() const;
    TransformComponent* GetChild(size_t i) const;

    Matrix4x4     GetLocalMatrix() const;
    Matrix4x4     GetWorldMatrix() const;     // 上溯链相乘
    Vector3       GetWorldPosition() const;
    Quaternion    GetWorldRotation() const;
    Vector3       GetWorldScale() const;
};
```

- 新增字段必须进 `Transfer`，否则旧 .zasset 无法读。**不破坏旧资产**：parent/children 在旧文件里读到默认值即可。
- `worldPositionStays` 行为对齐 Unity：换 parent 时世界位姿保持不变 → 重算 local。

---

## 6. 分阶段实施计划

| Phase | 内容 | 预估改动量 | 是否可独立合并 |
| --- | --- | --- | --- |
| **0. Transform Hierarchy** | parent/children + SetParent + 世界变换 + 单测 | ~300 行 | ✅ |
| **1. Runtime PrefabAsset + Instantiate** | `PrefabAsset` 类 + 反射注册 + 克隆算法 + `PrefabUtility::Instantiate` + AssetManager 集成 + 简单单测/示例 `.zasset` | ~800 行 | ✅ |
| **2. Editor PrefabInstance + Modifications** | `PrefabInstance` + `PropertyModification` + Apply/Revert + Added/Removed Components + ComputeModifications + 编辑器菜单挂接 | ~1200 行 | ✅ |
| **3. Nested + Variant + Unpack + Merge** | Flatten 三层叠加 + 嵌套 PrefabInstance 支持 + Variant base 链 + Unpack 模式 + 端到端测试 | ~800 行 | ✅ |
| **4. 收尾** | 文档示例、CHANGELOG、构建验证、修复回归 | ~200 行 | ✅ |

每个 Phase 完成都跑一次完整 `cmake --build ... --parallel` 验证。

---

## 7. 风险与决策记录

1. **PropertyModification 的 propertyPath 用什么 DSL？** —— 对齐 Unity 的「点分路径 + 数组 [n]」（如 `m_LocalPosition.x`、`m_Components.Array.data[2].m_Color.r`）。需要反射系统提供 `ResolvePathForRead/Write(Object*, string)` API；现有 `Type/SerializeUtility` 不直接提供，需要 Phase 2 补一个轻量 path resolver（基于 Transfer 名字 + 数组索引）。
2. **GUID 来源** —— 现有 AssetManager 走的是「path → SerializedFileIndex」，没有显式 GUID。Phase 1 增加 `.zasset.meta`（json）为每个 prefab 文件生成稳定 GUID，作为 `PPtr` 跨文件持久化的真名。如果时间紧，第一版用 path 哈希作 GUID 占位，后续替换。
3. **InstanceID 复用问题** —— `Instantiate` 必须**严格分配新 InstanceID**；Component 内部对 GameObject 的反向指针 (`m_parent_object`) 不要参与 `Transfer`，靠 `postLoadResource()` 重建（现有代码已经这么做了）。
4. **Editor-only 字段隔离** —— 用现成的 `TransferMetaFlags::HideInEditorMask`/编辑模式开关；不引入 `WITH_EDITOR` 宏污染 Runtime 代码。`PrefabInstance` 类型整个放 Editor 模块，Runtime 不引用。
5. **场景里的 PrefabInstance 在 player 构建里如何处理？** —— Player（非 editor）构建时 **不打包 `PrefabInstance` 类型**，构建工具在 cook 阶段把每个 PrefabInstance flatten 成普通 GameObject 树写进 player 用的场景文件。Phase 3 之后把 cook 步骤补到 asset_pipeline。

---

## 8. 验收清单（功能 Done 的判据）

- [x] **Phase 0**：`TransformComponent` 支持 `SetParent`/`GetWorldMatrix`（已落地，`transform_component.cpp`）
- [x] **Phase 1**：能从 `.zasset` 读出 `PrefabAsset`，`Instantiate` 后场景里有完整 GO 树（`PrefabUtility.cpp`）
- [x] **Phase 1**：多次 Instantiate 互不干扰，InstanceID/GObjectID 全新分配（每次 ReadObject 都重新分配）
- [x] **Phase 2a**：`PrefabInstance` 骨架 + corresponding-source 映射 + 序列化（`PrefabInstance.h/.cpp`）
- [x] **Phase 2b**：实例修改字段并记录 `PropertyModification`（`RecordPropertyOverride` / `RecordCurrentValueAsOverride`）
- [x] **Phase 2b**：`ApplyAll` 把 instance overrides 写回 source object；`Revert` 拉源值回 instance；`RevertSingleOverride` 单字段回退
- [x] **Phase 2b**：`AddComponentOverride` / `RemoveAddedComponent` / `SuppressSourceComponent` / `UnsuppressSourceComponent` 全套
- [x] **Phase 3a**：嵌套 Prefab 通过 `PrefabRefComponent` 实现（运行时由 `PrefabPostLoadDriver::ExpandNestedPrefabsOn` 展开）
- [x] **Phase 3b**：Variant 实例化（`InstantiateVariant`）—— 走到链尾 base 后实例化，并把 `m_source_prefab` 指回 variant
- [x] **Phase 3c**：`UnpackPrefabInstance` 清理 bookkeeping 并保留 cloned 子树
- [x] **Phase 3d**：`MergeFromSource` 三层叠加（base → variant overrides → instance overrides，Phase 3 阶段 variant overrides 暂以 instance.m_modifications 通道叠加）
- [x] **Phase 4**：全量构建零 error/LNK/fatal（最后一次构建 `_phase3bcd_build.log` ExitCode=0）
- [ ] **后续**：示例 `.zasset` 文件 / 端到端单测 / 编辑器 UI 集成

---

## 9. 实现状态笔记 v2（2026-05 更新）

### 9.1 已上线的代码地图

| 模块 | 路径 | 关键类型 / 函数 |
| --- | --- | --- |
| Runtime Asset | `engine/Source/Runtime/Resource/Prefab/PrefabAsset.{h,cpp}` | `PrefabAsset`（含 `m_variant_base` / `kCurrentSchemaVersion`；**无独立 GUID 字段，跨文件引用走 PPtr**） |
| Runtime Utility | `engine/Source/Runtime/Resource/Prefab/PrefabUtility.{h,cpp}` | `PrefabUtility::Instantiate` / `InstantiateFromPath` / `SetInstantiatedRootParent`；内嵌 `PrefabPostLoadDriver` 含 `ExpandNestedPrefabsOn` |
| Runtime Component | `engine/source/runtime/function/framework/component/PrefabRefComponent.{h,cpp}` | 嵌套 Prefab 的安装点组件 |
| Editor 模型 | `engine/Source/Editor/Prefab/PrefabInstance.{h,cpp}` | `PrefabInstance` + `CorrespondingSourceEntry` + `m_added_components` / `m_removed_components` |
| Editor 模型 | `engine/Source/Editor/Prefab/PropertyModification.{h,cpp}` | `PropertyModification`（`value` 文本占位 + `value_bytes` eastl::string 装载真二进制 + `object_reference`） |
| Editor 路径 | `engine/Source/Editor/Prefab/PropertyPathResolver.{h,cpp}` | `TokenizePropertyPath`（点分路径 + `[n]`） |
| Editor 路径 | `engine/Source/Editor/Prefab/PropertyValueLocator.{h,cpp}` | `LocatePropertyInStream` / `OverwriteScalarBytes` —— 在 StreamedBinary 字节流上定位字段 |
| Editor 克隆 | `engine/Source/Editor/Prefab/EditorPrefabCloner.{h,cpp}` | `CloneSubtree` 结构化克隆（围绕 PPtr/ImmediatePtr Transfer 缺陷做指针重连） |
| Editor 主入口 | `engine/Source/Editor/Prefab/PrefabUtilityEditor.{h,cpp}` | `InstantiateAsPrefab` / `InstantiateVariant` / `RecordPropertyOverride` / `ApplyAll` / `Revert` / `RevertSingleOverride` / `AddComponentOverride` / `RemoveAddedComponent` / `SuppressSourceComponent` / `UnsuppressSourceComponent` / `UnpackPrefabInstance` / `MergeFromSource` |

### 9.2 实施过程发现的 ZEngine 设施差距

为完成 Prefab 系统暴露出的几个底层缺陷，已经原地修复：

1. **`PPtr<T>::Transfer` 是空实现**（`PPtr.h:97-99`）—— 普通 round-trip 序列化无法在内存里 deep-clone 含 PPtr 的对象。**绕过方案**：`EditorPrefabCloner` 自带的结构化克隆 + `ptr_map` 指针重写。
2. **`ImmediatePtr<T>::Transfer` 只写 `LocalSerializedObjectIdentifier`，不还原运行时指针** —— GameObject 的 `m_components` 字段 round-trip 后变全 null。**绕过方案**：`RebuildCloneComponents` 用 `ptr_map` 重新挂回。
3. **`SerializeTraits` 只为 int32/uint32/int64/uint64/char/float/bool 特化 `std::vector<T>`，没有 `uint8_t`** —— `PropertyModification` 的字节字段被迫改用 `eastl::string`，并提供 `BytesToStorageString`/`StorageStringToBytes` 桥接。
4. **`MemoryCacheReader::m_Memory` 原本是 `std::vector<uint8_t>` 值** —— 与 `MemoryCacheWriter` 已是引用语义不一致。改为引用，并新增 `TransferUtility::ReadObjectFromVector`、`CloneObjectViaSerialization`。
5. **`Component::m_parent_object` 无 getter** —— Suppress/Unsuppress 链需要从 Component 反查 host GO，已加 `Component::GetParentObject()`。
6. **`GameObject::getComponents()` 按值返回** —— 编辑器需要原地增删，已加 `EditorGetComponentsRef()` / `EditorRemoveComponent()`。

### 9.3 设计决策与简化

* **Variant 的 overrides 暂不烘进 `PrefabAsset`**。当前设计把 Variant 的 modification 当作运行期的 `PrefabInstance.m_modifications` 走 `MergeFromSource` 叠加；Variant `m_variant_base` 字段只用于 `InstantiateVariant` 时的 base 解析。后续如果需要"无 PrefabInstance 单独打开 Variant 资源"也能保持 override，再把 `PropertyModification` 搬到 Runtime 模块并在 PrefabAsset 里加 `m_variant_overrides` 字段。
* **嵌套 Prefab 通过 `PrefabRefComponent` 而非 PrefabInstance-as-object**。Unity 在 SerializedFile 内嵌 sub-PrefabInstance；ZEngine 改为更轻量的"Component 标记 + 运行时展开"，是否有展开代价由 `m_expanded` 标记控制（保存场景时把展开状态烘进去，避免双展开）。
* **`PropertyModification.value`（文本）保留但不走 Apply 通路**。所有 Apply/Revert/Merge 都用 `value_bytes` + `object_reference`。`value` 字段留作未来 YAML 调试或 .meta 文件输出用，避免 Phase 阶段就引入 string↔bytes 编解码层。

### 9.4 接下来可做（非阻塞）

1. ~~**`.zasset` 示例 Prefab + 端到端单测**~~ —— **已落地（2026-10-25）**，见下方 §9.4.1。
2. **PrefabInstance 自身序列化往返** —— **对象图层已验证（2026-10-25）**：PrefabInstance 与它的克隆子树作为一张对象图写进 `.zasset`（二进制）和 `.scene`（YAML）再读回，覆盖列表、corresponding-source 映射、实例根引用、跨文件的源 PrefabAsset 引用都保留（§9.4.1 的 P6）。**场景链路已接通（2026-10-26）**：`Level` 新增序列化钩子 `LevelSerializationHooks`（Runtime 侧只开扩展点，不认识 PrefabInstance），编辑器的 `PrefabSceneBinding`（`Editor/Prefab/PrefabSceneBinding.{h,cpp}`，`RegisterEditor()` 里 `Install()`）挂上三个钩子：`Level::save` 把根仍在关卡里的已登记实例写进同一个场景文件；`Level::LoadObjectGraph` 读回后把实例的 GameObject 引用从"文件里读出的 GameObject"改指到 Level 据此新建的活对象（组件是共享的，GameObject 不是）；`Level::Unload` 清登记。`PlaceInLevel(level, instance)` 是"把 Prefab 放进场景"那一步。见 §9.4.1 的 P7。
3. **编辑器 UI 集成** —— **Inspector 已接（2026-10-26）**。逻辑层 `Editor/Prefab/PrefabOverrideModel.{h,cpp}`（无 UI，端到端测试 P17 直接驱动）：
   - **隐式覆盖**（Unity 模型）：Inspector 每次提交字段后调 `SyncFieldOverride(instance, target, path)`，复合字段（如 `m_LocalScale`）拆成叶子，只给和源不同的叶子记覆盖，改回源值即删除覆盖；
   - `IsOverridden` 给行标签上色，`ListOverrides` 列出覆盖；`RevertField` / `ApplyField` / `ApplyAll`；
   - Apply 之后 `SaveSourcePrefab` 按**原 fileID** 重写源 `.prefab` / `.zasset`（场景对 Prefab 内对象的引用是 (文件, fileID)，重编号会悄悄指错），再对关卡里同源的其它实例 `MergeFromSource`（它们自己的覆盖保留）；
   - 覆盖列表进撤销：`MakeOverrideListUndoRecord`，和字段的 `RecordComponent` 同一个撤销步骤，列表没变时自动丢弃。
   
   ZSlate Inspector（`ZSlateInspectorWindow.cpp`）：选中 Prefab 实例里的物体时顶部出现 "Prefab <源文件>  N overrides" + Apply All / Revert All，下面每条覆盖一行带 Revert / Apply；被覆盖字段的标签是蓝色；Transform 的位置 / 旋转 / 缩放和通用字段的提交都走同步。**界面只做了 clang-cl 类型检查，没上机看过**。ZSlate 没有右键菜单设施，所以没做成右键菜单。Apply 改的是源文件，和 Unity 一样不进撤销。
   
   内容浏览器拖 Prefab 进场景**已接（2026-10-26）**：`EditorScenePlacement` 改走 `PrefabSceneBinding::InstantiateIntoLevel`（= `InstantiateAsPrefab` + `PlaceInLevel`），一个撤销步骤；以前那条路径（`InstantiateFromPath` + 只 `CreateObject` 根）只把根放进关卡，子 GameObject 挂在根的 Transform 下却不是关卡对象，也不留 Prefab 关联。
4. ~~**Cook 阶段 flatten**~~ —— **已落地（2026-10-26）**。场景里的 GameObject 树本来就是实例化 + 覆盖后的实体（`PrefabSceneBinding` 保存的就是它），所以 flatten = 去掉簿记对象，不需要重新展开。`Editor/Build/SceneCook.{h,cpp}`：`CookSceneFile` 去掉所有 `kTypeIsEditorOnly` 类型的对象，并裁掉只被它们引用的外部引用（源 `.prefab`），其余对象里的外部引用索引随之重编号；指向被删对象的引用置空并计数。纯文本层（`ZYaml::StripObjectsFromGraph`），不 Produce 对象，编辑器里开着同一个场景时打包也不受影响。`AssetBundleBuilder` 对 `.scene` 在源文件比烘焙副本新时重烘焙到 `Intermediate/Cooked/<平台>/` 并打包烘焙副本。只处理 YAML 场景；二进制关卡原样拷贝并告警。
   - 顺带修：`REGISTER_CLASS_TRAITS(kTypeIsEditorOnly)` 以前是**死元数据**——`IMPLEMENT_REGISTER_CLASS` 只把 `kTypeIsAbstract` 传进运行时 `Type`。现在 `Type::isEditorOnly` / `IsEditorOnly()`（末位成员带默认值，既有的位置初始化列表不受影响）。
   - ~~打包器的依赖遍历仍按源场景走~~ —— **已改（2026-10-26）**：依赖遍历抽成 `Editor/Build/BuildDependencyWalk`，`.scene` 先烘焙、再从烘焙副本读依赖（`AssetRegistry::GetDependenciesOfFile`），只被 PrefabInstance 引用的 `.prefab` 不再进包。做这一步时发现 **YAML 场景在 AssetRegistry 里根本没有依赖**：依赖索引只认二进制 SerializedFile 头，YAML 文件读失败就当成零依赖——也就是打包一个 `.scene` 时它引用的 Prefab / 材质 / 网格**都不会**跟着进包。现在 YAML 资产从 externals 表取依赖（`ZYaml::ReadExternalRefs`）。另外两处：扫描时边扫边建依赖，场景排在它引用的资产前面时（目录序）那条边被永久丢掉，改为整轮扫完再建；`AssetRegistry::ScanAssets` 原来是空函数，现在是同步扫描。见 P16。

#### 9.4.1 端到端测试落地记录（2026-10-25）

`engine/Source/Editor/Prefab/Test/PrefabEndToEndSmokeTest.cpp`（276 项断言；P1-P6 首版 58 项，P7-P17 于 2026-10-26 补），走真实代码和真实磁盘：

| 场景 | 内容 |
|---|---|
| P1 | 搭 root + child（各带 Transform）→ `PrefabUtility::SaveAsPrefabAsset` 写 `.zasset` |
| P2 | `PrefabUtilityEditor::InstantiateAsPrefab(path)` 从磁盘读回 → 结构克隆：名字、层级、本地位置、corresponding-source 映射 |
| P3 | 改实例字段 → `RecordCurrentValueAsOverride`（同一属性重录是覆盖不是追加）→ `ApplyAll` 写进源、清空覆盖列表、不碰未覆盖字段 |
| P4 | 把应用后的源另存一份 `.zasset`，以全新对象读回：应用的值落盘了 |
| P5 | `Revert` 把未应用的覆盖拉回源值，不改源 |
| P6 | PrefabInstance + 克隆子树按场景对象图写二进制 / YAML 再读回（见上面第 2 条） |
| P7 | 真实场景链路：`PlaceInLevel` → 改值录覆盖 → `Level::save`（YAML + 二进制）→ 读回 `Level::LoadObjectGraph`：实例重新绑定、根/子/映射全指向新关卡的活对象、覆盖值和覆盖列表在、**读回后 Revert 仍能拉到源值**；根被删（撤销缓冲区仍持有）时不写实例、恢复后又写；`Unload` 清登记 |
| P8 | 对多对象文件里非头对象的惰性 PPtr 解引用：返回的是那个对象本身（以前返回文件头 PrefabAsset，类型都不对） |
| P9 | 同一文件的相对路径 / 绝对路径 / `./` 写法得到同一个实例 ID |
| P10 | P7 换成 YAML 源 Prefab（`.prefab`）：场景存读（YAML + 二进制）后实例仍连得上源、Revert 有效 |
| P11 | YAML Prefab 的非头对象惰性解引用：整图加载、按 ID 取到；之后 `LoadPrefabAsset` 复用这张图而不是再读一份，且能直接实例化 |
| P12 | 同一 ID 下先后 Produce 两个对象，销毁被取代的第一个不能注销第二个的表项 |
| P14 | 运行时 `PrefabUtility::InstantiateFromPath` 每次返回独立的一棵树（YAML / 二进制 Prefab 都测）：两次调用不共享对象、不交出缓存的模板图、改一个不影响另一个；`m_DefinitionUrl` 展开（`invoke_post_load=false`）两次不拿到同一组组件 |
| P15 | 内容浏览器拖放路径 `InstantiateIntoLevel`：同一 Prefab 拖两次 = 两个独立的关联实例，4 个 GameObject 都是关卡对象，两个都会保存；拖不存在的 Prefab 失败且不动关卡 |
| P16 | 打包依赖：YAML 场景到二进制 / YAML Prefab 的边进了 AssetRegistry；Player 遍历经烘焙副本，只被 PrefabInstance 引用的 Prefab 不进、被普通对象引用的进；新鲜的烘焙副本不重烘；没有烘焙目录时退回源依赖；`ScanAssets` 同步扫描（4 处变异全红） |
| P17 | Inspector 覆盖模型：只记和源不同的叶子、改回即删、重复同步不重复记；覆盖列表撤销 / 重做 / 无变化不成步；单字段 Revert 不碰别的覆盖；单字段 Apply：源值、源文件（fileID 不变）、另一个实例合并到新值且保留它自己的覆盖；ApplyAll（6 处变异全红） |
| P13 | Player 烘焙：`PrefabInstance` 带编辑器专属标志、运行时类型不带；带覆盖的场景烘焙后没有 PrefabInstance、没有指向源 `.prefab` 的外部引用，烘焙副本能加载、两个 GameObject 和覆盖值都在、不绑定实例；手写对象图验证外部引用裁剪 + 重编号、悬空引用置空（4 处变异全红） |

**运行方式**：`bash scripts/verify_prefab_e2e.sh`。无原生工具链时它用 `scripts/web_smoke_run.py`：借 build-web 里已配置的 smoke-test 目标的编译/链接命令，把测试和 `Editor/Prefab/*.cpp` 链到 web 版 `libZRuntime.a`（`--whole-archive`），补上 WebGL2 / emmalloc / NODERAWFS 链接参数后用 node 跑。原生构建可用 `-DZENGINE_BUILD_PREFAB_E2E_SMOKE_TEST=ON`。

**这个测试第一次跑起来就暴露的真 bug（都已修，变异验证过测试能抓住）**：

1. **磁盘上的 Prefab 根本无法实例化**：`InstantiateAsPrefab(path)` 和 `PrefabUtility::InstantiateFromPath`（二进制 `.zasset` 分支）都用 `AssetManager::ReadObject<PrefabAsset>`，它只读 fileID 1 这一个对象；头里的 `m_RootGameObject` 指向同文件的 fileID 2，单对象读永远不会产出它 ⇒ 根恒为 null。新增 `PrefabUtility::LoadPrefabAsset(path, out_gameobjects)` 读整张对象图（YAML / 二进制统一），两个入口都改走它。
2. **克隆只克隆根**：`EditorPrefabCloner` 的遍历只认识"已访问过的 GameObject"的 Transform 归属，永远迈不出根到第一个子节点那一步。改为沿 `Component::m_ParentObject` 广度优先；`LoadPrefabAsset` 为读出的模板图只绑定归属指针（新增 `Component::BindOwnerIfUnbound`，不跑 PostLoadResource，避免把源资产注册成场景活对象）。
3. **克隆的父节点子列表重复**：序列化拷贝把源的 `m_Father` / `m_Children` 一起带过来，`SetParent` 又往里追加 ⇒ 克隆父节点同时列出源子节点和克隆子节点。新增 `Transform::ClearCopiedHierarchyLinks()`，重连前清掉。
4. **`Transform::Transfer` 读写不对称**：读时先 `m_LocalPosition` 后 `m_LocalPositionD`（写反过来），且读时多探测 `transform` / `m_parent` / `m_children` 三个旧字段。按名字查找的读取器（SafeBinaryRead / YAML / JSON）不受影响，但**按位置读的 StreamedBinaryRead**（Prefab 克隆、覆盖快照都用它）读错位 ⇒ 克隆的位置是垃圾。另外 GenerateTypeTreeTransfer 既非读也非写，会走"读"分支把旧字段写进 TypeTree。改为读写同序，旧字段只给按名字查找的读取器探测。
5. **Apply / Revert / Merge 对 Transform 无效**：它们用 `ReadObjectFromVector` 原地覆写序列化字段，但不通知对象；Transform 的 TRS 实际读自层级缓冲区，覆写后看不到变化。三处统一改走 `ReadBackInto`：覆写后调 `Component::OnSerializedFieldsUpdated()`（Undo 和 Inspector 用的同一约定）。
6. **外部引用的路径可能存空**：`StoreExternalAssetPath` 用 `std::filesystem::absolute`，取不到当前目录时返回空路径，跨文件引用下次加载就静默变 null（node/wasm 下必现，原生上 cwd 被删时也会）。改为回落到原路径。
7. **web 构建读不了磁盘上的 `.zasset`**：`LocalFileSystemWeb::Size` 对未打开的条目返回 0，`FileCacherRead` 恰恰是先用未打开的条目取大小 ⇒ SerializedFile 头校验失败。改为未打开时 `stat()`（与 Linux / Windows 实现一致）。这也让既有的 `YamlRoundTripSmokeTest` Y9 二进制场景在 web 上转绿。

**P7-P9 暴露并修掉的问题（2026-10-26，变异验证：5 处各撤一处，测试都转红）**：

8. **同一文件两种写法 = 两个身份**：路径→序列化文件索引表按字符串原样查（`RuntimeAssetManager` 原样、`EditorAssetManager` 只做 lexically_normal），而索引是路径派生实例 ID 的一半。用相对路径加载的 Prefab，被 YAML 场景写成绝对路径的外部引用，读回时铸出另一个 ID ⇒ 指向一份不存在的副本，读回的场景里 Revert / Apply 找不到源。新增 `AssetManager::CanonicalPathKey`（纯词法：相对路径锚到当前目录 + 规范化），两个实现都按它查，不同写法登记为别名；存储的路径名不变（播放器里 bundle 按原写法查）。二进制场景没暴露是因为它存的是 `lexically_normal` 后的原写法，恰好一致。
9. **惰性 PPtr 解引用多对象文件的非头对象，返回的是文件头**：`AssetManager::ReadObject(instanceID)` 的各分支都只读 fileID 1。改为 fileID≠1 时：二进制文件且整图未加载 ⇒ `ReadObjectsFromBinary` 加载整图后按 ID 取；否则返回 null 并告警（YAML 图用堆 ID，永远对不上路径派生 ID；整图已加载时再读会在同 ID 下重复 Produce）。
10. **活 GameObject 没有实例 ID**：`Level::CreateObject` 用 `make_shared` 建的 GameObject 从不分配实例 ID，任何 PPtr 都无法指向它，场景写出时也映射不到 fileID。现在建好即 `AllocateAndAssignInstanceID`（ObjectManager 表不持有所有权，`~Object` 注销 ID，shared_ptr 仍是唯一所有者）。
11. `Level::clear` 无条件解引用 `GET_SYSTEM(PhysicsManager)`，工具 / 无物理环境下卸载即崩；加空检查。`Level::load` 拆出 `LoadObjectGraph(entries, create_physics_scene)`，测试和工具可以不建物理场景（web 构建无线程，Jolt 的 job system 起不来）。

**P10-P12 暴露并修掉的问题（2026-10-26，5 处变异全红）**：

12. **YAML 资产读出的对象没有文件身份**：`ReadObjectsFromYaml` 用全新的堆 ID Produce 对象，于是 ①场景引用 `.prefab` 里的对象时写成 null（堆 ID 查不到所属文件），读回的场景 Revert / Apply 全部失效；②从文件铸出的 PPtr 永远对不上已加载的对象。`ZYaml::ReadObjectGraph` 加可选的实例 ID 钩子，`AssetManager::ReadObjectsFromYaml` 传路径派生 ID——与二进制读取器一致。纯文本（无文件）的读取不传钩子，行为不变。
13. 随之：惰性解引用 YAML 图的非头对象改为整图加载（原先对 YAML 直接返回 null）；`LoadPrefabAsset` 的"已加载就复用"从只认二进制扩到 YAML，且复用时若图是被惰性解引用加载的（没走过模板准备：重建组件列表 + 绑定归属）就补做。
14. `Object::~Object` 无条件注销自己的 ID：同一文件被读两次时第二份对象接管表项，销毁第一份会把第二份的表项删掉 ⇒ 之后对它的 PPtr 解引用失败或触发重读。改为只在表项仍指向自己时注销（`ObjectManager::UnregisterInstanceID(id, expected)`）。

**P14 暴露并修掉的问题（2026-10-26）**：

15. **运行时实例化交出的是模板本身**：`PrefabUtility::Instantiate` 的约定是"每次返回独立的新树"，实现却是"在资产的图上跑一遍 post-load 然后返回它的根"，靠的是"每次都从磁盘重读"。二进制 `.zasset` 早就被缓存（路径派生 ID 不能重读），所以**两次实例化同一个二进制 Prefab 拿到的是同一组对象**；`d1a51a5e` 把 YAML 也改成缓存后，`.prefab` 也一样——**这一步是我上一提交引入的回归**（`GameObject::ExpandDefinition` 让所有引用同一 `m_DefinitionUrl` 的场景对象共享同一组组件实例）。修：结构克隆器从 `Editor/Prefab/EditorPrefabCloner` 挪到 `Runtime/Resource/Prefab/PrefabSubtreeCloner`（它只用 Runtime 类型；命名空间 `prefab_clone`），`Instantiate` 克隆缓存的模板后再跑 post-load，模板不动；`InstantiateFromPath` = `LoadPrefabAsset` + `Instantiate`。变异（恢复成交出模板）⇒ P14 六项红。

**未验证**：编辑器里真的拖一个 Prefab 进场景、在 Inspector 上改值后 Apply、保存场景再打开（UI 链路本身是第 3 条，未接）。`Level::load(url)` 带物理场景的完整路径未在无头环境跑过（它与 `LoadObjectGraph` 的差别只在建物理场景）。


---

## 附录 A：Unity 调研要点对照表（仅供参考）

| Unity 概念 | 实现位置（Unity 源码） | ZEngine 对应 |
| --- | --- | --- |
| `Prefab` (旧) | Runtime/BaseClasses/PrefabBackwardsCompatibility.h | 不实现，纯历史包袱 |
| `PrefabInstance` | Editor/Src/Prefabs/PrefabInstance.h | `Editor/Prefab/PrefabInstance` |
| `PropertyModification` | Editor/Src/Prefabs/PropertyModification.h | `Editor/Prefab/PropertyModification` |
| `PrefabUtility::InstantiatePrefab` | Editor/Src/Prefabs/PrefabUtility.cpp | `Runtime/Prefab/PrefabUtility::Instantiate` |
| `PrefabUtility::SaveAsPrefabAsset` | Editor/Src/Prefabs/PrefabUtility.cpp | `Editor/Prefab/PrefabSave.cpp` |
| `CorrespondingObjectFromSource` | EditorExtensionImpl::m_CorrespondingSourceObject | `PropertyModification.correspondingSource` + `PrefabInstantiateContext` |
| `m_PrefabAsset` | EditorExtensionImpl 字段 | `PrefabInstance.sourcePrefab` |
| Variant base 链 | PrefabAsset 自身字段 | `PrefabAsset.variantBase` |

> 本文档以 Unity 2023.1 在 `e:/Engine/unity2023.1` 的源码为参考；具体到 ZEngine 上的语义有调整（去除历史 API、合并 Editor-only/Runtime 边界、`.zasset` 二进制取代 YAML）。

**P16-P17 暴露并修掉的问题（2026-10-26）**：

16. **YAML 场景没有依赖**（见 §9.4 第 4 条）：AssetRegistry 的依赖索引只读二进制头。
17. **AssetRegistry 依赖边与扫描顺序有关**：扫描循环里每个文件一入表就解析它的外部引用，引用目录序更靠后的资产时查不到、边被丢弃。改为整轮登记完再建边（异步扫描与 `IncrementalUpdate` 两处）。
18. **零位置经 Apply / Revert / Merge 回不去**：覆盖按叶子逐个拼进字节流再读回，`Transform::Transfer` 的读取分支在双精度位置为零、旧 float 副本非零时回退到旧副本——而拼完最后一个叶子时旧副本还是上一次的值，结果位置变回原值。这条回退只对按名字查找的读取器有意义（旧数据里根本没有 `m_LocalPositionD` 字段），按位置读的 StreamedBinaryRead 拿到的总是写入器成对写出的两份，现在只在前者上回退。
