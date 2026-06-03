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

1. **`.zasset` 示例 Prefab + 端到端单测**：构造一个含 GO + Transform + 几个简单 Component 的 PrefabAsset，写入磁盘 → reload → Instantiate → 改字段 → ApplyAll → reload PrefabAsset 验证修改持久化。
2. **PrefabInstance 自身序列化往返**：当前 PrefabInstance 已经 `IMPLEMENT_OBJECT_SERIALIZE`，但场景文件保存/加载链路还需要把 PrefabInstance 作为顶层 Object 落到 SerializedFile 里。
3. **编辑器 UI 集成**：把 `RecordPropertyOverride` / `Apply` / `Revert` 接到 Inspector 的右键菜单。
4. **Cook 阶段 flatten**：Player 构建时把 PrefabInstance flatten 成普通 GameObject 树（参见 §7.5）。


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
