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

实现 = `PrefabAsset` 多两个字段（**都在 Runtime 侧**，2026-10-27 落地）：

```cpp
ZPROPERTY(Name="m_variant_base")
PPtr<PrefabAsset> m_VariantBase;               // 非空表示这是 Variant
ZPROPERTY(Name="m_variant_overrides")
std::vector<PropertyModification> m_VariantOverrides; // 相对 base 链、本层改了什么
```

⚠️ **Variant 不拥有对象树**：它没有 root GameObject，只有"base 是谁 + 本层改了什么"。

实例化（Runtime `PrefabUtility::Instantiate`，Editor `PrefabUtilityEditor::InstantiateVariant` 共用同一条通路）：

1. `ResolveVariantChain` 走链到**最深非 variant 的 base**（>64 环或链断裂直接失败，不静默退化成"渲染 base"）
2. 克隆**那一棵**树（整条链共用这一份克隆）
3. 用 `CloneResult::ptr_map`（Runtime）/ `corresponding_source`（Editor）把 override 的 `target` 从 base 图翻译到克隆体
4. **top-down**（base → … → variant）逐层 `ApplyVariantOverrides`，同一字段上近的覆盖远的
5. `PostLoadResource`

因为链上每一环的 `target` 都指向**同一个最深 base** 图里的对象，任意长度的链都能套在同一份克隆树上。

PrefabInstance 引用 Variant 时，再叠加 `PrefabInstance.m_modifications` —— 三层叠加顺序：**base → variant overrides → instance overrides**（与 Unity 一致）。

创建入口（2026-10-27）：Content Browser 右键 `.prefab` → *Create Prefab Variant*，在 base 旁边生成 `<stem> Variant.prefab`（重名自动 `_1`/`_2`），零 override。Variant 的 Variant 也允许。

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
- [x] **Phase 3d**：`MergeFromSource` 三层叠加（base → variant overrides → instance overrides，2026-10-27 改为经 `SnapshotEffectiveSource` 读"base 字节 + 整条 Variant 链的 override"，不再只认裸 base 或只认叶子；组件结构由 `ReconcileStructure` 先行校正）
- [x] **Variant 真·继承（2026-10-27）**：`PropertyModification` 下沉 Runtime + `PrefabAsset::m_variant_overrides`（schema 1→2）；`PrefabUtility::Instantiate` 沿 base 链展开并 top-down 应用 override，近的赢；`LoadPrefabAsset` 对"无 root 但 IsVariant()"放行；`CreateVariantAsset`/`SaveVariantAsset` 落盘；Editor 的 `InstantiateVariant` 改为复用同一条 Runtime 通路；Content Browser 右键 `.prefab` → *Create Prefab Variant*。测试 `ScriptComponentPrefabTest` I 组（单层）/ J 组（base←mid←leaf 三层链）/ K 组（落盘再读回，两轨各 11 项），checks 100 → 158 全绿。
- [x] **Variant 可写（2026-10-27）**：覆盖的拼接提到字节层（`PrefabUtility::SpliceModificationIntoBytes` / `SnapshotWithOverrides`），Apply 到 Variant 时 target 换回 base 图对象并 upsert 进 `m_variant_overrides` + `SaveVariantAsset`，写盘失败保留 modifications；`Revert` / `RevertSingleOverride` / `MergeFromSource` 的源快照改走 `SnapshotEffectiveSource` 因而认得 variant 层；`PrefabOverrideModel::ApplyField` / `ApplyAll` 按 `SourceIsVariant` 分派。测试 `ScriptComponentPrefabTest` L 组 10 项（含 L6：Apply 之后 base 模板对象本身没被改），checks 158 → 168 全绿。
- [x] **Variant 结构性覆盖（2026-10-27）**：`PrefabAsset` 追加 `m_variant_removed_components`（base 组件的 PPtr）与 `m_variant_added_components`（`VariantComponentAddition` = variant 文件内的组件对象 + base 宿主 GO），schema 2→3；`SaveVariantAsset` 把新增组件作为额外对象一起写盘；`PrefabUtility::ApplyVariantStructuralChanges` 在实例化时先删后加，新增组件必须克隆；`ApplyAllToVariant` 顺带提交 `m_added_components` / `m_removed_components` 并在写盘成功后一并清空。测试 M 组 32 项（两轨各 12，含"两次实例化拿到的是两个独立组件"），checks 168 → 202 全绿，2 处变异验证各转红。
- [x] **Variant 结构性覆盖的编辑器侧（A8，2026-10-27）**：`ReconcileStructure` 让 `InstantiateVariant` / `MergeFromSource` / `Revert` 对组件结构的认识一致；链级净结构（后层可删前层新增）；override 可指向 Variant 新增的组件；单字段 Apply 不再泄漏结构；Apply 后收养活组件；`SaveVariantAsset` 保持 fileID 稳定；Inspector 列出加/删组件行（`can_apply` 控制 Apply）。测试 E2E P19（53 项）+ `ScriptComponentPrefabTest` N 组（34 项，两轨）= 236 + 339；11 处变异全红；同时挖出并修掉 `BaseRenderer` 空指针与 `AssetRegistry` 反向索引两个 web 轨看不到的 bug（§9.4.1 19、20）。
- [x] **Phase 4**：全量构建零 error/LNK/fatal（最后一次构建 `_phase3bcd_build.log` ExitCode=0）
- [ ] **后续**：示例 `.zasset` 文件 / 端到端单测 / 编辑器 UI 集成

---

## 9. 实现状态笔记 v2（2026-05 更新）

### 9.1 已上线的代码地图

| 模块 | 路径 | 关键类型 / 函数 |
| --- | --- | --- |
| Runtime Asset | `engine/Source/Runtime/Resource/Prefab/PrefabAsset.{h,cpp}` | `PrefabAsset`（含 `m_variant_base` / **`m_variant_overrides`** / **`m_variant_removed_components`** / **`m_variant_added_components`**（`VariantComponentAddition` = 文件内组件 + base 宿主 GO）/ `kCurrentSchemaVersion`（=3）；**无独立 GUID 字段，跨文件引用走 PPtr**） |
| Runtime Utility | `engine/Source/Runtime/Resource/Prefab/PrefabUtility.{h,cpp}` | `PrefabUtility::Instantiate` / `InstantiateFromPath` / `LoadPrefabAsset` / `SetInstantiatedRootParent`；Variant：`ResolveVariantChain` / `ApplyVariantOverrides` / **`ApplyVariantStructuralChanges`** / **`ResolveVariantStructure`** / **`CollectVariantLayerOverrides`** / `CreateVariantAsset` / `SaveVariantAsset` / `SpliceModificationIntoBytes` / `SnapshotWithOverrides`；内嵌 `PrefabPostLoadDriver` 含 `ExpandNestedPrefabsOn` |
| Runtime 模型 | `engine/Source/Runtime/Resource/Prefab/PropertyModification.{h,cpp}` | `PropertyModification`（`value` 文本占位 + `value_bytes` **std::vector\<uint8_t\>** 装载真二进制 + `object_reference`）—— 2026-10-27 从 `Editor/Prefab/` 下沉，命名空间 `prefab_editor` → `prefab` |
| Runtime 路径 | `engine/Source/Runtime/Resource/Prefab/PropertyPathResolver.{h,cpp}` | `TokenizePropertyPath`（点分路径 + `[n]`）—— 同上，2026-10-27 下沉 |
| Runtime 路径 | `engine/Source/Runtime/Resource/Prefab/PropertyValueLocator.{h,cpp}` | `LocatePropertyInStream` / `OverwriteScalarBytes` —— 在 StreamedBinary 字节流上定位字段；同上，2026-10-27 下沉 |
| Runtime Component | `engine/source/runtime/function/framework/component/PrefabRefComponent.{h,cpp}` | 嵌套 Prefab 的安装点组件 |
| Editor 模型 | `engine/Source/Editor/Prefab/PrefabInstance.{h,cpp}` | `PrefabInstance` + `CorrespondingSourceEntry` + `m_added_components` / `m_removed_components` |
| Editor 克隆 | `engine/Source/Editor/Prefab/EditorPrefabCloner.{h,cpp}` | `CloneSubtree` 结构化克隆（围绕 PPtr/ImmediatePtr Transfer 缺陷做指针重连） |
| Editor 主入口 | `engine/Source/Editor/Prefab/PrefabUtilityEditor.{h,cpp}` | `InstantiateAsPrefab` / `InstantiateVariant` / **`CreateVariant`** / `RecordPropertyOverride` / `ApplyAll` / **`ApplyAllToVariant(include_structure)`** / `Revert` / `RevertSingleOverride` / `AddComponentOverride` / `RemoveAddedComponent` / `SuppressSourceComponent` / `UnsuppressSourceComponent` / **`ReconcileStructure`** / `UnpackPrefabInstance` / `MergeFromSource` |
| Editor 覆盖模型 | `engine/Source/Editor/Prefab/PrefabOverrideModel.{h,cpp}` | `SyncFieldOverride` / `IsOverridden` / `ListOverrides`（含 `OverrideRow::Kind` 与 `can_apply`）/ **`HasOverrides`** / `RevertField` / **`RevertStructuralRow`** / `ApplyField` / `ApplyAll`（后两者按 `SourceIsVariant` 分派到 `ApplyToSource` / `ApplyAllToVariant`） |
| Content Browser | `engine/Source/Editor/EditorUI/ContentBrowser/ContentBrowserAssetActions.{h,cpp}` | `CreateNewAsset`（Prefab / Material / …）+ **`CreatePrefabVariant`**（右键 `.prefab` → *Create Prefab Variant*） |

### 9.2 实施过程发现的 ZEngine 设施差距

为完成 Prefab 系统暴露出的几个底层缺陷，已经原地修复：

1. **`PPtr<T>::Transfer` 是空实现**（`PPtr.h:97-99`）—— 普通 round-trip 序列化无法在内存里 deep-clone 含 PPtr 的对象。**绕过方案**：`EditorPrefabCloner` 自带的结构化克隆 + `ptr_map` 指针重写。
2. **`ImmediatePtr<T>::Transfer` 只写 `LocalSerializedObjectIdentifier`，不还原运行时指针** —— GameObject 的 `m_components` 字段 round-trip 后变全 null。**绕过方案**：`RebuildCloneComponents` 用 `ptr_map` 重新挂回。
3. ~~**`SerializeTraits` 没有 `uint8_t` 特化**~~ —— **前提本身是错的（2026-10-27 更正）**：`SerializeTraits<uint8_t>` 一直存在（`SerializeTraits.h:57`，当初就是为 `Texture2D::m_Pixels` 加的），配 `SerializeTraits<std::vector<T>>` 泛化特化可直接用。原先把 `PropertyModification::value_bytes` 写成 `eastl::string` 当字节袋，代价是 **YAML 轨静默损坏**：YAML 把 string 写成标量，遇到以 `0x00` 开头的载荷就截断成空串，而这几乎涵盖所有载荷（小端 `double 42.0` = `00 00 00 00 00 00 45 40`）。表现是 `.zasset` 正常、`.prefab` 的 override 读回来变成 0 字节，日志里只有一句 `value size 0 != field size 8`。已改为 `std::vector<uint8_t>`，`BytesToStorageString`/`StorageStringToBytes` 那层桥接随之删除，两轨行为一致。
4. **`MemoryCacheReader::m_Memory` 原本是 `std::vector<uint8_t>` 值** —— 与 `MemoryCacheWriter` 已是引用语义不一致。改为引用，并新增 `TransferUtility::ReadObjectFromVector`、`CloneObjectViaSerialization`。
5. **`Component::m_parent_object` 无 getter** —— Suppress/Unsuppress 链需要从 Component 反查 host GO，已加 `Component::GetParentObject()`。
6. **`GameObject::getComponents()` 按值返回** —— 编辑器需要原地增删，已加 `EditorGetComponentsRef()` / `EditorRemoveComponent()`。

### 9.3 设计决策与简化

* ~~**Variant 的 overrides 暂不烘进 `PrefabAsset`**~~ —— **已推翻并落地（2026-10-27）**。原方案把 Variant 的 modification 当作运行期的 `PrefabInstance.m_modifications` 走 `MergeFromSource` 叠加，但 `PrefabInstance` 是 `kTypeIsEditorOnly`，所以真机 `GameObject.Instantiate(variantPath)` 只会拿到 base 的裸拷贝 —— 编辑器里"Variant"看起来有用，打包后完全没有。现在：
  * `PropertyModification` 已从 `Editor/Prefab/` 下沉到 `Runtime/Resource/Prefab/`（命名空间 `prefab_editor` → `prefab`）；
  * `PrefabAsset` 新增 `m_variant_overrides`（`kCurrentSchemaVersion` 1 → 2，追加在末尾，老文件照旧可读）；
  * `PrefabUtility::Instantiate` / `PrefabUtilityEditor::InstantiateVariant` 都走同一条 Runtime 通路 `PrefabUtility::ApplyVariantOverrides`，编辑器与真机由构造保证一致。
* **Variant 不拥有对象树**。一个 Variant 文件里只有 PrefabAsset 头、没有 root —— `LoadPrefabAsset` 因此对"无 root 但 `IsVariant()`"放行，否则 Variant 根本存不成资产。实例化 = 走链到最深非 variant base → 克隆那一棵树 → 用 `ptr_map`（Runtime）/ `corresponding_source`（Editor）把 override 的 target 翻译成克隆对象 → 逐层 top-down 应用。**链上每一环的 target 都指向同一个最深 base 图里的对象**，所以任意长度链都能套在同一份克隆树上，近的覆盖远的。
* **嵌套 Prefab 通过 `PrefabRefComponent` 而非 PrefabInstance-as-object**。Unity 在 SerializedFile 内嵌 sub-PrefabInstance；ZEngine 改为更轻量的"Component 标记 + 运行时展开"，是否有展开代价由 `m_expanded` 标记控制（保存场景时把展开状态烘进去，避免双展开）。
* **`PropertyModification.value`（文本）保留但不走 Apply 通路**。所有 Apply/Revert/Merge 都用 `value_bytes` + `object_reference`。`value` 字段留作未来 YAML 调试或 .meta 文件输出用，避免 Phase 阶段就引入 string↔bytes 编解码层。
* **覆盖的拼接在字节层做，不在对象层做**（2026-10-27 新增）。base Prefab 的文件里那组对象是**模板**：运行时 `Instantiate` 每次都克隆它，编辑器里所有该 base 的实例也共享它。凡是"读一个应用了覆盖之后的值"的需求（Revert / RevertSingleOverride / MergeFromSource 拉源值、Apply 写回前的合并），都不得往模板对象身上 splice —— 那会污染每一个实例，而且模板不在 Variant 文件里，写也写不出去。统一做法是：序列化 → 在字节流上按路径覆写 → `ReadBackInto`（反序列化 + `OnSerializedFieldsUpdated`）。这条原语同时是 Runtime 应用 override（`ApplyModification`）和 Editor 三个 Revert/Merge 入口的底座，所以放在 Runtime 的 `PrefabUtility` 上。
* **Variant 新增的组件存在 Variant 文件里**（2026-10-27 新增）。变体不拥有对象树，所以"加一个组件"这件事没法只记成"往 base 的某个 GO 上挂一个东西"——那个组件对象本身得有个去处。方案是让 Variant 文件携带它：写盘时 `SaveVariantAsset` 把 header 加每个 addition 的组件一起交给 writer（fileID 1 是 header，2..N 是新增组件），header 用 `ImmediatePtr` 指过去，`LoadPrefabAsset` 整图读回时一并解出来（M 组两轨都验证过）。**这些对象和 base 的模板一样是共享的**，所以实例化时也必须克隆，不能交出去 —— 否则两次拖同一个 Variant 会拿到同一个组件实例，就是 P14 那个"两次拖同一个 prefab 共享一个 Transform"的翻版。相应的：
  * `SaveVariantAsset` 不再是"永远只写一个对象"，它按 addition 数量带 N 个对象；
  * `LoadPrefabAsset` 缓存分支从 fileID 2 往上遍历时遇到 Component 不会中断（只有 `IDToPointer` 返回 null 才 break），所以多对象的 Variant 文件照旧能命中缓存。
* **结构性变更在属性覆盖之前应用**（2026-10-27 新增）。`Instantiate` 里的顺序是：结构性变更（先删后加）→ 属性覆盖 → `PostLoadResource`。倒过来会让 PostLoadResource 走到一个本该被 Variant 删掉的组件上，或者错过一个本该已经挂好的新增组件（TypeScriptComponent 的 `BindAndAwake` 就发生在 post-load 里）。

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
4. ~~**Prefab Variant**~~ —— **已落地（2026-10-27）**，采用模型 A（真·继承）。此前它是**死代码**：`SetVariantBase` 零调用者，`IsVariant()` 恒 false，`InstantiateVariant` 永远退化成 `InstantiateAsPrefab`，1365 行端到端测试零覆盖。落地内容：
   - 存储：`PropertyModification` 等三份从 `Editor/Prefab/` 下沉到 `Runtime/Resource/Prefab/`；`PrefabAsset` 加 `m_variant_overrides`（schema 1→2）。**没有选 `PrefabInstance.m_modifications`** —— 它是 `kTypeIsEditorOnly`，真机拿不到，等于"编辑器里看着有用、打包后没有"；
   - 实例化：`PrefabUtility::Instantiate` 走链到最深非 variant base → 克隆它的树 → `ptr_map` 翻译 target → top-down 应用（近的赢）。Editor 的 `InstantiateVariant` 改用同一条 Runtime 通路，两边由构造保证一致；
   - 落盘：Variant 文件只有一个 PrefabAsset 头、没有 root，`LoadPrefabAsset` 因此对"无 root 但 `IsVariant()`"放行；`CreateVariantAsset` / `SaveVariantAsset` 写入；
   - 入口：Content Browser 右键 `.prefab` → *Create Prefab Variant*；
   - 顺带修掉一个真 bug：`value_bytes` 原是 `eastl::string` 当字节袋，YAML 写标量遇到 `0x00` 开头就截断 —— 小端 `double 42.0` 正是 `00 00 00 00 00 00 45 40`，所以 `.prefab` 轨的 override 读回来是空（`.zasset` 轨正常，只有 authoring 轨坏）。改为 `std::vector<uint8_t>`。
   - 测试：`ScriptComponentPrefabTest` I 组（单层 variant + 直接实例化 base 的对照）/ J 组（`base←mid←leaf` 三层链，x 取近的、y/z 保留远的）/ K 组（落盘再读回，`.prefab` 与 `.zasset` 两轨各 11 项），checks 100 → 158 全绿。**编辑器部分本机只能 `-fsyntax-only`**（`bin/` 里没有 editor 库，链不了），`Editor/Prefab` 四份 TU + Content Browser 两份 TU 语法通过。
   - ~~**Variant 可写**~~ —— **已落地（2026-10-27）**。上面那轮只是"Variant 能读"，override 仍然只能从代码里手工填：Inspector 改字段时 `RecordPropertyOverride` 只写 `PrefabInstance.m_modifications`，`m_variant_overrides` 恒空，从菜单建出来的 Variant 永远是零 override。更糟的是 **Apply 对 Variant 是破坏性的**——`ApplyAll` 会把覆盖 splice 进 base 的对象，而 base 的对象是所有该 base 实例共享的模板（且不在 Variant 文件里），于是污染每一个实例，`SaveSourcePrefab` 随后还会拒绝写入。修法是把"把覆盖拼进**字节**而不是**对象**"抽成共享原语：
     - `PrefabUtility::SpliceModificationIntoBytes(bytes, tree, mod)` —— 在一份已序列化的字节流上按路径定位字段并覆写（`TokenizePropertyPath` → `LocatePropertyInStream` → `OverwriteScalarBytes`）。`ApplyModification`（实例化通路）改为复用它；
     - `PrefabUtility::SnapshotWithOverrides(source, mods, out_tree, out_bytes)` —— 序列化 `source`，再把属于它的 mods 全 splice 进去，得到"source 叠上 variant 层之后的值"。**全程不碰 `source` 对象**；
     - `PrefabUtilityEditor::SnapshotEffectiveSource` 在 Editor 侧把它包装成"base 字节 + variant 层"，`Revert` / `RevertSingleOverride` / `MergeFromSource` 三处的源快照都改用它 —— 于是这三个操作第一次认得 variant 层（以前它们读的是裸 base，Revert to Variant 实际上会 revert 到 base）；
     - `PrefabUtilityEditor::ApplyAllToVariant(instance)` —— target 从实例对象换回 **base 图里的对象**，upsert 进 `m_variant_overrides`，`SaveVariantAsset` 成功后才 `ClearModifications()`（写盘失败返回 0 且保留 modifications）；`PrefabOverrideModel::ApplyField` / `ApplyAll` 按 `SourceIsVariant` 分派。
   - 测试：新增 L 组 10 项，checks 158 → 168 全绿。其中最关键的一条是 **L6：Apply 到 Variant 之后，源（base）对象本身的值没变**——共享模板没被污染。编辑器侧仍只有 `-fsyntax-only`。
   - ~~**Variant 结构性覆盖**~~ —— **已落地（2026-10-27）**。上面那轮只覆盖了"改值"，加/删组件仍只有 `PrefabInstance` 那一份（`m_added_components` / `m_removed_components`），Variant 的永远为空。难点在"加组件"：Variant 不拥有对象树，那个组件对象得有去处 —— 解法是让 Variant 文件携带它（header 在 fileID 1，新增组件在 2..N，`SaveVariantAsset` 一起写，header 用 `ImmediatePtr` 指向），于是 `SaveVariantAsset` 不再是恒写单对象。实例化时新增组件**必须克隆**（它和 base 模板一样是共享的，交出去就是 P14 的翻版），顺序是先删后加、且整体在 `PostLoadResource` 之前。测试 M 组 32 项（两轨各 12），其中 M11 / M12 钉的就是"拿到的不是模板对象本身"和"两次实例化拿到两个独立组件"。2 处变异各转红。
   - ~~**结构性覆盖的 Revert / MergeFromSource / Inspector**~~ —— **已落地（A8，2026-10-27）**，见 §9.4.1 的 21–26。
   - ~~结构性 Revert 没进撤销栈~~ —— **已落地（2026-10-28）**，见 §9.4.1 的 29。
   - **尚未做**：① ~~结构性 Revert 没进撤销栈~~（已做）；② **普通 prefab 的结构性 Apply**——要给源图加新对象、分新 fileID，Inspector 对这类行只给 Revert；③ **结构性覆盖的 ZSlate 界面没上机看**，只过 `-fsyntax-only`；④ ~~Camera / Particle 里同型的空指针~~（已修，§9.4.1 30）；⑤ ~~相机默认参数没有 instance ID~~（已修，§9.4.1 32）。

5. ~~**Cook 阶段 flatten**~~ —— **已落地（2026-10-26）**。场景里的 GameObject 树本来就是实例化 + 覆盖后的实体（`PrefabSceneBinding` 保存的就是它），所以 flatten = 去掉簿记对象，不需要重新展开。`Editor/Build/SceneCook.{h,cpp}`：`CookSceneFile` 去掉所有 `kTypeIsEditorOnly` 类型的对象，并裁掉只被它们引用的外部引用（源 `.prefab`），其余对象里的外部引用索引随之重编号；指向被删对象的引用置空并计数。纯文本层（`ZYaml::StripObjectsFromGraph`），不 Produce 对象，编辑器里开着同一个场景时打包也不受影响。`AssetBundleBuilder` 对 `.scene` 在源文件比烘焙副本新时重烘焙到 `Intermediate/Cooked/<平台>/` 并打包烘焙副本。只处理 YAML 场景；二进制关卡原样拷贝并告警。
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

**原生验证（2026-10-27）**：没有 emcc 的机器（macOS 开发机）用 `bash scripts/verify_prefab_native.sh [all|e2e|runtime]`，把同一批测试当真正的原生进程跑。它依赖 `bin/libZRuntime.a`（上一次 cmake 构建的产物，往往已过期）+ `build/Generated/ZHT/ZRuntime/Src`（ZHeaderTool 输出），所以脚本里每个"自上次构建后改过/新增的源"都以 `--extra-source` 现编、同时 `--exclude-archive-member` 把归档里的旧成员踢掉（否则符号重复）。症状是"代码明明对、测试却看到旧行为"时，十有八九是漏了一个文件——两处都要加。

**为什么 web 轨之外必须有原生轨**：wasm 的线性内存低地址是可寻址的，**空指针解引用不 trap**。web 轨一直是绿的，原生轨第一次跑就挖出两个"web 上永远不会红"的 bug（见下 19、20）。另外 `-undefined dynamic_lookup` 会让缺失的符号延迟到运行期——崩溃报告在 `~/Library/Logs/DiagnosticReports/*.ips`，用脚本解析崩溃线程栈（JSON，`usedImages` + `threads[triggered]`），比在沙箱里起 lldb 好用。

已知"失败"：`P18/types: expected to round-trip the registered runtime types` —— 它要求 >20 个已注册运行时类型，而单个冒烟二进制只链进 ~9 个，是覆盖下限而非行为检查；脚本据此判定（`[FAIL]` 数 > 这一条的数量才算失败）。

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

**P19 / 原生轨暴露并修掉的问题（2026-10-27，11 处变异全红）**：

19. **`BaseRenderer::SubmitRenderState` 解空指针**：`GET_SYSTEM(RenderSystem)->GetSwapContext()` 无保护，任何无渲染系统的进程（工具、测试、服务端）实例化一个带 MeshRenderer 的 Prefab 就 SIGSEGV（`0x10` = swap-data 偏移）。web 轨不 trap，P18 一直静默写 `0x10` 而通过。已加空检查直接返回。**同型未动**：`CameraComponent.cpp` ×5、`ParticleComponent.cpp` ×1（不在 Prefab 链路上，本机也无法验证相机/粒子路径）。
20. **`AssetRegistry::IndexDependenciesLocked` 重新索引会抹掉别人对它的引用**：它先调 `RemoveDependencyEdgesLocked(self)`，后者除了删自己的**出边**，还 `m_ReferencerMap.erase(self)`——那是指向自己的**入边**，属于别的资产。结果取决于目录遍历顺序：APFS 先列 `keep.scene` 再列 `hero.zasset`，`keep.scene → hero.zasset` 这条反向边被后索引的 `hero.zasset` 抹掉，`GetDependencies` 有而 `GetReferencers` 没有。拆出 `RemoveOutgoingEdgesLocked` 给重索引用，`RemoveDependencyEdgesLocked` 只留给删除。web 轨目录序不同，P16 一直绿。

**A8：Variant 结构性覆盖的 Revert / Merge / Apply / Inspector（P19）**。A7 让 Variant 能携带加/删组件，但编辑器侧 Revert 和 `MergeFromSource` 完全没看结构层（头文件注释从 Phase 3 起就写着"Detects added/removed components"，函数体从来没做）。要点：

21. **`ReconcileStructure(instance)`**：按"源 + 链的净结构 + 实例自己的加/删"校正实例的组件结构——① 摘掉源已不再拥有的（链删除的 / 链已不再新增的），② 克隆链的新增（实例没有对应、也没自己 suppress 的），③ 重新引入没人再 suppress 的 base 组件。`InstantiateVariant` / `MergeFromSource` / `Revert` 三处都走它，所以对结果的认识一致。所有引入都经 `IntroduceComponentFromSource`：克隆后**叠上链在该组件上的属性 override**（以前 Unsuppress 克隆的是裸源值，Variant 对它的修改丢了），并进对应表。
22. **链级解析**：`PrefabUtility::ResolveVariantStructure` 算整条链的净结构（后层可以删前层新增的组件）；`CollectVariantLayerOverrides` 取整条链的属性层。顺手修掉 A6 遗留的一个洞：`SnapshotEffectiveSource` 只取**叶子** Variant 的 override，所以 `base ← mid ← leaf` 上 mid 设置而 leaf 没动的字段，Revert 会回到 **base** 值而不是 mid 值。Runtime 的 `ApplyVariantStructuralChanges` 同时改为读写 `source_to_clone`：新增组件的"模板 → 克隆"也登记进去，这样**属性 override 能指向 Variant 新增的组件**（目标是 variant 文件里的模板对象，不在 base 图里）。
23. **`ApplyAllToVariant(include_structure)`**：单字段 Apply 不再顺带提交实例的结构覆盖（A7 里应用一个字段会把整个实例的加/删组件一起写进 Variant）；删"自己这一层新增的组件"时撤销那条 addition（记一条 removal 没有效果——同一层内 removal 先于 addition 解析，组件会原样回来）；Apply 之后把实例里**活的**组件与新模板配对，否则下一次 Merge / Revert 会在宿主上再克隆**第二份**；写盘失败回滚 Variant 的三个列表（否则内存里的 Variant 与文件分叉，下一次 Merge 会泄漏到其它实例）。
24. **`SaveVariantAsset` 保持 fileID 稳定**：A7 每次都按 1..N 重排，删掉一个 addition 后其后的模板全部前移一位，场景里对它们的 (文件, fileID) 引用静默指错。现在已有身份的对象沿用原 fileID，新对象接在最大值之后（同 `SaveSourcePrefab` 的规则）。
25. **`SuppressSourceComponent` 摘掉后清对应表项**：以前"有对应"不再蕴含"在宿主上"，Unsuppress 又加一条，一个 suppress/unsuppress 来回留下两条配对。
26. **Inspector**：`ListOverrides` 现在也列加/删组件行（以前只有结构覆盖时头部显示 "0 overrides" 且没有按钮）；`OverrideRow::can_apply` 决定是否显示 Apply（普通 prefab 没有结构性 Apply——要给源图加新对象、分新 fileID——所以只给 Revert）；组件行的 Revert 走 `RevertStructuralRow`；撤销记录只录属性行（RemovedComponent 行的 target 是共享的源模板，撤销绝不能往里写）。

测试：P19 编辑器全链路 53 项（创建 Variant → 实例化 → suppress/add → 行模型 → 逐行 Revert / Revert All → Apply All → 新实例 / 旧实例 Merge → 实例自己的结构在 Merge 后保留 → 单字段 Apply 不泄漏 → 普通 prefab 只给 Revert → Variant 层在重新引入时不丢），`ScriptComponentPrefabTest` N 组 34 项（链级删除前层新增、override 指向新增组件，`.prefab` / `.zasset` 两轨）。**变异验证 11 处全红**：Revert/Merge 不 reconcile、ApplyField 提交结构、Suppress 留脏配对、Apply 不收养活组件、ListOverrides 丢组件行、重新引入丢 Variant 层、后层删除被忽略、模板不登记进映射、只算最后一层。其中**第一版 M7（重新引入丢 Variant 层）存活**——测试里我猜的属性路径根本解析不了，Variant 层从没被应用，"重新引入时是否带层"就没被测到；改用 `shared_materials` 整数组覆盖后才转红。**教训：变异存活 = 测试前提本身没成立，先断言前提（P19/layer 的 precondition）。**

27. **路径失败自带原因（`prefab::ExplainPathFailure`）**：`override splice` / `SpliceFieldValueInto` / `RecordPropertyOverride` / `RecordCurrentValueAsOverride` 的失败日志原先只有 "path not found"，A8 连续三次因此猜错（`m_ScriptGuid` 实为 `m_script_guid`、`m_Scale` 实为 `m_scale`、`shared_materials[0]` 定位器根本不支持）。现在日志里带原因，例如 `no field 'm_DoesNotExist' under '<root>' (available: enabled, m_LocalRotation, ...)`；对非数组用 `[n]`、畸形路径、数组处给了字段名也各有说法。它只看 TypeTree、不需要流字节，所以流本身坏掉时也能用。**注意**：它报 "resolves" 时真正的 locator 仍可能失败——locator 跨不过变长元素数组的下标，那种情况只能整数组覆盖。测试 N-diag 1–4 钉住这几种输出，3 处变异全红。

28. **P18 的三层对称检查（原生 `types` 模式，67 类型 / 342 checks）**：① 位置读消费字节数 == 写出字节数；② 逐叶子植入（单个定长标量，抓『放错字段 / 丢字段』；double 位置与其 float 副本按结构规则放行）；③ **整流合成**——直接从 TypeTree 造一个填满的流（数组 2 元素、字符串 3–5 字符、嵌套结构递归；对齐按**绝对流位置**补 4 字节），位置读入再写出逐字节比较，抓『动态内容』与『值域』：Wide 模式每个标量每个字节都非零且互不相同（符号位/高字节置位），Edge 模式 −0.0 / 非规格化 / ±FLT_MAX / INT_MIN / INT_MAX。默认对象载荷几乎全 0、数组字符串全空，前两层读不出『位宽错、丢尾、反序、字符串截断、−0.0 被规范化』。变异验证 10 个（vector<string> 反序/丢尾、字符串截断、vector<struct> 丢尾/反序、float 清低位字节、uint8 清高位、−0.0 规范化、非规格化清零、范围钳位），**全部只被第 ③ 层抓住**；W1 只在 wide、E1/E2/E3 只在 edge 模式转红，两种取值缺一不可。
    **合成器自己的两个缺陷（都先被当成引擎 bug，值得记）**：(a) 键字节生成周期只有 5，map 的两个键逐字节相同，map 合法地合并成 1 项；(b) `unordered_map` 读回的元素顺序由容器自己决定（MemoryProfiler.CSharp.data 两个元素原样回来、顺序对调），所以 map 只放 1 个元素，多元素顺序由普通 vector 覆盖。另：前提断言第一版只数『访问过的数组』，生成器把所有数组置空它也绿——改为只数有元素的数组、再加元素总数下限后才被抓住。**限制**：只覆盖 TypeTree 能表达的布局；引用字段（m_FileID/m_PathID）置零不植入；map 的多元素顺序不检查；无序列化字段的 7 个脚本绑定类（Input/Mathf/Time/…）默认流为空，逐个打印跳过。

29. **结构性 Revert 进撤销栈（P20）**：新增 `StructureRecord`（`PrefabOverrideModel::MakeStructureUndoRecord` / `StructureUndoKey`），交换语义：保存实例每个 GameObject 的**组件列表** + `m_added_components` + `m_removed_components` + 对应表。Inspector 的 Revert All 与组件行 Revert 在同一事务里提交它和属性覆盖记录，一次 Ctrl+Z 一起回退。**能安全交换裸指针的前提**：Revert 路径上没有任何地方销毁组件——摘掉的组件仍归对象表所有，重新引入的克隆在 undo 后只是被摘下『停放』，redo 再挂回；代价是这些停放的克隆活到关卡卸载。若销毁它们，redo 就做不到。`Finalize` 在结构没变时返回 false，所以什么都没改的 Revert 不产生撤销步。**已知限制**：undo 只恢复列表，不重跑挂接时的钩子——撤销带回来的 MeshRenderer 已在组件列表里，但渲染提交（`PostLoadResource` 里的 `SubmitRenderState`）要等下次属性改动才会重新触发；这是未上机验证的点。测试 P20（~30 项，真 `UndoSystem`）：Revert All 一步、undo 恢复形状/列表/配对且是**同一个**对象、redo 恢复同一个克隆、反复 undo/redo/undo 稳定、无变化不产生步、逐行 Revert 可撤销、键与覆盖列表键不撞。**8 个变异全红**（不存组件列表 / 不存 added / 不存 removed / 不存对应表 / Finalize 恒 false / 恒 true / 合并键为 0 / 键撞车）；其中合并键为 0 只被『HasPendingRecord 报告』一条抓住，不是漏检——撤销按相反顺序应用，第二份记录即使没被忽略也会被最早那份覆盖回去，键只影响内存和快速跳过。

30. **CameraComponent / ParticleComponent 的同型空指针（P21）**：§9.4.1 的 19 修了 `BaseRenderer`，同型还剩 `CameraComponent` ×5（`PostLoadResource` / `OnSerializedFieldsUpdated` / 三个 `Tick*Camera` 的渲染交接）和 `ParticleComponent` ×2（`Tick` 的 `RenderSystem`、`PostLoadResource` 的 `ParticleManager`——后者是无头 prefab 实例化真正会走到的）。修法：相机用一个 `TryGetSwapContext()`，**只跳过『交给渲染器』这一步**，相机自己的状态与角色旋转照常更新（Tick 里不能整体 return）；`OnSerializedFieldsUpdated` 里 `WorldManager` 也补了空检查（该处原本只防了『没有活动关卡』）。测试 P21 先**断言前提**——进程里确实没有 RenderSystem / WorldManager / ParticleManager——再走会崩的路径。4 个守卫逐个还原成原样，**4 个全红**（崩溃）。
    **变异第一次存活，原因是个独立的发现**：把相机交接的守卫全部还原成无保护，测试仍然是绿的。因为 `createDefaultCameraParameter()` 用 `MemoryManager::CreateObject` 造默认参数，它**没有 instance ID**，而 `PPtr<CameraParameter>(ptr)` 以 instance ID 为键，于是默认参数包进 PPtr 后读回是 **null**——`PostLoadResource` 因此在『camera parameter is null』处提前返回，渲染交接那段在这条路径上是**死代码**（完整引擎里也一样）。测试改为用 `ObjectManager::Produce` 造一个有身份的参数，并先断言 `GetCameraParameter() == cam_param`（前提）；之后守卫还原才转红。**这个默认参数的身份问题本身没修**，是否该改成 `ObjectManager` 分配需要你决定（涉及 §工程规则里的分配器配对）。
    `verify_prefab_native.sh`：两个源加入重编清单；`types` 模式下 P21 若被跳过（Camera/Particle 没注册）直接判失败，否则这条回归保护会悄悄消失；默认 `e2e` 模式只注册约 9 个类型，P21 在那里可见地跳过。

31. **全仓扫描：无保护 `GET_SYSTEM(X)->`（176 处 / 53 个文件）的分级结论**：不全改——先按『无头加载路径上会不会执行』分级。测试进程里实际存在的只有 ObjectManager / AssetManager / FileSystem；Render / World / Particle / Input / Window / Config / DebugDraw / RHI 都不存在。
    - **Tick 类函数**（`CameraComponent::Tick*`、`MotorComponent::TickPlayerMotor`…）只在引擎主循环里被调用，那时这些系统必然存在，**不属于加载路径，不改**。
    - **`RigidBodyComponent::PostLoadResource`**：真问题。组件自己的注释写着『prefab 展开的 body 每次关卡加载都会走到这里』，并已处理『还没有物理场景』，却漏了『根本没有 WorldManager』。7 处取场景的调用收敛成 `CurrentPhysicsScene()`；3 处 `ASSERT(physics_scene)`（显式 `CreateRigidBody` / 查询接口的**前置条件**）原样保留，没有弱化。析构 / `RemoveRigidBody` / `GetActivePhysicsScene` 先判了 `m_RigidbodyId`（无头实例化下 body 从没建成），本来就走不到，也一并走了辅助函数。**还原守卫后测试崩溃，被抓住。**
    - **`TimelineComponent::PostLoadResource`**：同型，已加空检查；**但没有被测试保护**——默认组件的资产路径为空，`LoadTimelineAsset()` 立刻返回 false，WorldManager 那行在默认状态下**不可达**；把守卫还原成无保护，测试仍是绿的（变异存活，如实记录）。要覆盖需要真实 `.timeline` 文件 + 设置私有 `m_TimelineRes`，成本明显高于它要保护的 3 行空检查，所以选择不硬凑。P21 里该断言的措辞已改成它真实证明的东西。
    - `MotorComponent::PostLoadResource` 的 `ObjectManager`：`RegisterCore` 注册，所有进程都有，不改。
    - **剩下约 150 处未逐个看**（AssetManager 26、WorldManager 21、WindowSystem 19、InputSystem 11…，多在渲染 / 窗口 / 输入层）：它们不在 prefab 加载路径上，本轮没有证据说它们会在无头进程里被执行，所以**没有改动**。这是『没发现问题』而不是『确认没有问题』。

32. **相机默认参数的创建与销毁（方案 A，P22）**：§9.4.1 30 发现 `createDefaultCameraParameter()` 用 `MemoryManager::CreateObject` 造的默认参数**没有 instance ID**，`PPtr<CameraParameter>(ptr)` 读回 null，`PostLoadResource` 在 "camera parameter is null" 处提前返回，渲染交接是死代码；同时没有任何地方销毁它，**每个相机泄漏一个**。现在：① 改用 `ObjectManager::NewObject<T>()`（真实的负 instance ID，PPtr 可解析）；② `CameraComponent` 加析构，`ReleaseOwnedDefaultParameter()` 只释放**自己创建的**那一个（`m_OwnedDefaultParameterId`，按 ID 查找而非按 PPtr），用 `MemoryManager::DestroyObject` 释放——`NewObject` 是 mimalloc，这是它的正确配对；③ 外部传入（`Initialize`）或文件读出的参数**不归相机管**，`Initialize` 替换时只释放被替换掉的默认；④ 模式切换（`OnSerializedFieldsUpdated`）换新默认时释放旧默认。
    **克隆是整个设计里最容易漏的一环**：克隆（prefab 实例化、复制粘贴）经过序列化往返，PPtr 原样写入 instance ID，于是克隆和原件指向**同一个**参数，原件一销毁克隆就悬空。所以 `PostLoadResource` 里 `DetachFromParameterOwnedElsewhere()` 查一张 `参数ID → 拥有它的相机` 的登记表（匿名命名空间里的 side table，没有改可序列化的 `CameraParameter`），发现参数属于另一个相机就用 `NewObject` 复制一份私有的（**不用** `CloneObjectViaSerialization`：它走 `Produce`/`new`，和 `DestroyObject` 不配对）。
    测试 P22（真 `ObjectManager`）：默认参数可解析、类型正确；克隆拿到**自己的**参数且值被复制（fov 77 而非默认 50）；销毁原件释放其参数而克隆的不受影响；销毁克隆释放克隆的；外部参数不被释放；外部参数**替换**已有默认时释放默认；模式切换释放旧默认。**7 个变异：6 个被抓住，1 个等价**。其中 A4（`Initialize` 不释放旧默认）第一次存活——因为原测试先 `Initialize` 再 `PostLoadResource`，此时没有旧默认可泄漏，前提没成立；补了 5b『先有默认、后被替换』才转红。A3（`Release` 里不清零 ID）是**等价变异**：堆 ID 只递减、从不复用（`m_NextHeapInstanceID`），二次进入只会查到已注销的 ID。清零保留为防御性写法，注释里写明，不为让变异转红而硬凑测试。
    **没解决的**：登记表按相机指针记录，**相机在不同线程并发 PostLoadResource 的顺序**没测过（表有互斥）；`CharacterController` / `MotorComponent` 用 `NewObject` 造的 `RigidBodyShape` / `Capsule` 看起来有同样的泄漏，我只读了代码没追查到底，**未改**。
33. **本机回归的已知断点（2026-10-29 起因，2026-10-29 晚已修好，见末尾"已解决"段）**：`verify_prefab_native.sh runtime` 与 `check_ts_chain.py` 在 rebase 到上游脚本层改动（`33e0e33`，ScriptingManager / 编辑器脚本 P0–P5）之后**失败**，且在**不含相机提交**的 `33e0e33` 上复现——归因是上游改动 + 本机陈旧的 `bin/libZRuntime.a`：先是 33 个重复符号（现编的 `ScriptEnv`/`ScriptBinding` 等没排除归档里的同名成员，已补），补完后 `ScriptingManager::Initialize` 跳到地址 0。用新加的 `run_smoke_test.py --link-map` 求得测试真正链入的归档成员 225 个，其中 **35 个自归档以来改过**（含 `RenderSystem` / `WindowSystem` / Lumen / Nanite…，因为 `RegisterRuntime` 的 `REGISTER_SYSTEM` 把整个引擎的系统都拉进来了），重编它们会引入 Objective-C++ / Metal 头文件（`NSObjCRuntime.h`），本机编不过——所以**没有**把它们接进脚本。根治只能是新的 cmake 构建出新归档，本机没有 cmake/ninja。仍然可用的：`verify_prefab_native.sh e2e|types`（407 项）不受影响，P18–P22 都在这两段里。**受影响的：runtime 套件（N 组 34 项、链级结构）和 TS 全链路，在这台机器上现在无法验证。**

    **已解决（2026-10-29 晚，装上 cmake 4.4.5 + 已有的 ninja 1.13.2 之后）**：真凶不是"陈旧归档"这个含糊说法，而是两个具体的坑，修完 `runtime` + `check_ts_chain.py` 双双恢复：
    * **fresh configure 本身就编不过**：`generate_engine_log.cmake` 生成 `struct BQ_EBCO : public ...`，但仓里记录的 bqlog 子模块（Release_2.4.1）里那个宏叫 `EBCO`（`b534ea2` 是按更新后的 bqlog 改的名）。`BQ_EBCO` 没有定义 → 被当成结构体名 → 37 个 category 每个都 redefinition → **ZEnginePCH 先挂**。修法是在生成头里桥接（有 `BQ_EBCO` 用它，否则回落 `EBCO`，再否则置空），不再依赖子模块指针。
    * **"刷新归档"根本没生效**：`run_smoke_test.py` 的 `prepare_pruned_lib_dir` 把 `bin/libZRuntime.a` 复制到 `/tmp/zengine_libs_<key>` 做缓存，**缓存键只按排除列表算、不看归档内容**。重编归档后仍在链重编前那份副本——表现是"插桩进了归档却没进二进制"。缓存键改为带上归档的 `mtime_ns + size`。
    修好后用 cmake 重编 ZRuntime（453 个源里 451 个编过，只有两个 Spine 源编不过，见下条），把 367 个对象写回归档（含 24 个此前归档里根本没有的新源对象，如 `NetworkSystem.cpp.o`）。
    **当前状态**：`verify_prefab_native.sh all` 三节全绿（runtime `RESULT: ALL PASSED` / e2e 420 checks 0 failures / types 444 checks 0 failures），`check_ts_chain.py` 242 checks 0 failed。
    **最后的卡点也已清掉（2026-10-29）**：`Runtime/CMakeLists.txt` 要求 `engine/Libraries/GPUSpine`，而 `.gitmodules` 里**从没有这一项** —— 报错信息明明写着
    `git submodule update --init engine/Libraries/GPUSpine`，可这条命令注定失败，于是任何 fresh clone 都 configure 不过。现已登记为子模块（pin `4aa1951`）。
    补齐之后还暴露出搬家时的漏改：`Component/Spine/SpineRenderer.cpp` 仍在 include `Runtime/Function/Render/Spine/SpineGpuAnimationState.h`（两个头已随 GPUSpine 搬走），
    改成 `GPUSpine/...` 后 **`ninja -C build ZRuntime` 退出 0**，cmake 自己完成归档重链，手工 `ar -r` 与 `spine_is_broken_upstream` 过滤一并退役。

34. **MotorComponent / CharacterController 的泄漏（P23）**：§9.4.1 32 里的『疑似』现在是**实测**的。`MotorComponent::PostLoadResource` 用 `NewObject` 造 `Capsule`，`CharacterController::Initialize` 再 `NewObject` 造 `RigidBodyShape`，二者只被 `PPtr` 持有、没有任何地方释放（`~CharacterController() = default`）；而且 `PostLoadResource` 可重入（prefab 重实例化），旧控制器被直接覆盖——**第二个**泄漏点。测试先红：重载累积、销毁不释放，3 项失败（前提『计数器看得到这些对象』的断言是过的，所以红是真泄漏）。
    修法：`CharacterController` 成为 `Capsule` 与 `RigidBodyShape` 的所有者（`Initialize` 接管传入的 capsule，文档里写明），析构与重入时用 `MemoryManager::DestroyObject` 释放（`NewObject` 的配对）；拷贝构造/赋值删除（拥有一对裸堆对象，拷贝即双释放）。`MotorComponent` 重载时先 `delete` 旧控制器，析构**改按指针判断**而不是按 `m_ControllerType`（指针才是『有没有东西要释放』的真相，`delete nullptr` 安全）。通过 `PPtr` 解析而非缓存裸指针来释放：已销毁的堆对象解析为 null，重复释放是 no-op。
    **7 个变异：全部抓住，但其中 2 个第一次存活，且原因不同于上次**：M5（重新采用自己已持有的 capsule 时仍释放它→use-after-free）我**事先预判**会存活——测试里每次都给新 capsule，`keep_capsule` 分支没人走；M4（`Initialize` 重入不释放）我**没预判到**：`MotorComponent` 重载时已经把整个控制器 `delete` 了，`new CharacterController()` 永远是首次 `Initialize`，里层的重入路径经由外层**根本不可达**——两层防护，外层先挡掉了。补了直接对同一个 `CharacterController` 调两次 `Initialize` 的用例（换新 capsule / 重新采用同一个）才都转红。
    **没覆盖到的**：`CharacterController::move` 的物理查询路径（`IsOverlap`）没有跑——本测试只管生命周期；`Capsule` 在 `PhysicsControllerConfig` 里是**内联值成员**（没有 instance ID），`MotorComponent` 是克隆值后交出去的，这点没改。

35. **普通 prefab 的结构性 Apply（P24）**：Variant 早就能提交新增/移除的组件，普通 prefab 不能——`ListOverrides` 把这些行标成 `can_apply=false`，
    `ApplyToSource` 直接跳过，于是实例上新增的组件只能 Revert，被抑制的组件永远无法真正从源里删掉。**为什么比 Variant 路径难**：Variant 只是往自己的侧表追加一条记录，
    普通 prefab 却要往**源图里塞一个新对象**——而源图里每个对象是按它已有的 fileID 写的，新对象还没有 fileID，`SaveSourcePrefab` 遇到没有 identity 的对象会拒绝**整个写入**
    （"a source object is not in <file>"）。所以新对象必须在保存**之前**先领 fileID：`GetInstanceIDFromPathAndFileID(path, id)` 拿到 ID，再用 `Produce(type, 该ID)` 造对象。
    不能用 `CloneObjectViaSerialization`：它自己挑 ID，和 fileID 对不上（变异 S9 验证过，它会让保存失败、整轮回滚）。

    两处**不看代码根本想不到**的坑：
    * `m_Component`（序列化清单）和 `m_Components`（活列表）是两个容器，`addComponent`/`EditorRemoveComponent` 只动后者；不调 `SyncSerializedComponents()` 的话，
      **文件里的 GameObject 仍是旧形状**，新组件要到**重新加载**时才消失——只看内存里缓存的那张图完全看不出来（这正是 S1 第一次存活的原因，补了『把文件复制一份重新加载』才转红）。
    * 被移除的组件对象**故意不销毁**：别的实例还拿它配对，Merge 要能解析到它；它只退出图，也就退出文件。于是它的 fileID 槽仍被占着，却**不在**『图内最大值』里。
      若上一次 Apply 移除的正好是最高 fileID 的那个，下一次新增就会抢它的槽——`Produce` 到那个已铸造的 ID 上会让所有指向旧组件的 PPtr 解析成**新对象**。
      分配规则：图内最大值 +1，再跳过仍被活对象占着的槽；测试把 doomed 挂在子节点上（它拿到最高 fileID）并分两轮 Apply（先只移除、再只新增）才覆盖到（S6）。

    保存失败会**回滚**（摘掉新挂的模板、把移除的挂回去、销毁新建对象），因为半应用的源会和文件不一致，下一次 Merge 会渗进所有实例。回滚路径本身**没测到**——本机造不出写失败。

    测试 P24：先红 12 项；实现后第一轮仍红 3 项（Merge），原因是**测试自己的问题**——实例没有 `PlaceInLevel`，`FinishApply` 的自动合并扫不到它，改成和 P19 一样显式 `MergeFromSource`。
    **10 个变异：9 个抓住，2 个先存活后补测转红，1 个等价**。

    ⚠️ **本条曾随 P22/P23 一起被上游 155ff0d 删掉**：该提交标题是 docs(gpuspine)，实际把 §9.4.1 32/33/34 的代码、测试与文档一并删除，
    2026-10-29 已按用户要求 cherry-pick 恢复。同一提交还删了 `engine/Source/Runtime/Function/Render/Spine/*` 的头文件，
    而 `SpineRenderer.cpp` 仍在 include 它们，于是本机 `types` 套件在纯净 main 上编不过（已加临时过滤：
    SpineRenderer 的 .cpp 与 .gen.cpp **两个都**留给归档，只留一半会在启动时 dyld "symbol not found"）。等上游修好 Spine 头文件后应移除过滤。
    **截至 3c4fdf8 仍未修复、过滤继续保留**：`Render/Spine/` 现在只剩 `SpineGpuRenderer.h/.cpp`（原 `SpineRenderer.h` 被改名），
    但 `SpineRenderer.cpp` 依旧 include 已不存在的 `SpineGpuAnimationState.h` 与 `SpineGpuAssetFile.h`——症状从"整目录被删"变成了"改名后漏了两个头"。S1（不 sync 序列化清单）与 S6（不跳过仍被占用的 fileID 槽）一开始都存活，都是**测试缺口**不是实现问题，
    分别靠『从文件重新加载』和『被移除的对象占着最高 fileID』补上；S10（从 header 而非图内最大值起算）是等价变异——在测试覆盖到的情形里两道防线的落点相同，
    只有『某个对象被销毁、槽空出来』时才会分叉，本机没有这种路径。

36. **结构性 Revert 进撤销栈（P25）**：用户要求做这个功能，但**先读 AGENTS.md §2.32 的结果显示它上游已经做完了**
    （`e8b81c9`，2026-10-28，143 行 P20 测试），而且逐条符合 §2.32：记录是**交换式**的（`Apply()` 把存储值与活值互换，天然自逆）、
    `Finalize()` 对"什么都没变"返回 false 从而丢弃空记录、只按 InstanceID 解析不用裸指针、组件快照经 `Transfer()` 落 YAML DOM 并配
    `ScopedPPtrResolver(nullptr)`、coalesce 键与属性记录区分（`"PSTR"`）、Inspector 在**改动之前**记录。因此没有重写，只补现状对不上测试的组合。
    ⚠️ 教训：**"未做"清单里的一项，动手前先读约定 + 查一遍代码**，否则会重复实现一遍已经存在且更合规的版本。

    **真正的缺口是 P24 之后的新组合**：普通 prefab 结构性 Apply（往源图里塞一个刚领到 fileID 的新对象）之后，
    再抑制这个源组件并 Revert、然后 undo/redo —— P20 只测了 Variant 路径，从没碰过"源对象是 Apply 现造的"这种情况。
    测试 P25 的流程：造普通 prefab（root 上一个 Kept、子节点上一个 Doomed）→ `InstantiateAsPrefab` →
    `AddComponentOverride` + `ApplyAll`（走 P24 那条路）→ `FindSourceForInstance` → `SuppressSourceComponent` →
    `BeginTransaction` + `AddRecord(MakeStructureUndoRecord)` + `AddRecord(MakeOverrideListUndoRecord)` + `Revert` + `EndTransaction`
    → 断言**只有 1 步**撤销 → Undo → Redo → 再来一轮 → 最后从"已撤销"的状态**再 Revert 一次**。

    ⚠️ **测试第一版转红是我自己写错了断言，不是实现有问题**：我断言"undo 之后活组件仍然配对"，而
    `SuppressSourceComponent` **故意**调了 `ErasePairForInstanceObject`——保留配对会让『有对应源』不再蕴含『还挂着』，
    Unsuppress 时还会把同一个源配对两次。所以**被抑制的组件按设计就是 UNPAIRED**。改成断言这个按设计的不配对状态，
    并且**加一条后果检查**：从撤销态再 Revert 一次必须恰好带回 **1 份**（不是 2 份也不是 0 份）——
    正是这条后果检查抓住了变异 M2（不交换 corresponding-source map），而"状态断言"单独一条抓不住。

    **4 个变异全部抓住，0 存活**：M1 不交换组件列表（P20×5 + P25×4）、M2 不交换对应源映射（P20×1 + P25/undo + P25/again）、
    M3 抑制时保留配对（P19×6 + P20×2 + P24×1 + P25×2）、M4 Revert 跳过 `ReconcileStructure`（P19×4 + P20×3 + P25×4）。
    检查数 e2e 420→448、types 444→472（+28）。

    顺带把 §2.32 补上与现状一致的一条：prefab 覆盖有**两个**自己的撤销记录（属性列表 / 组件结构），结构记录服务 Revert All 与单行 Revert；
    undo/redo 永不销毁组件（被摘下的先停靠、回滚时再挂回）；**结构性 Apply 刻意不入撤销栈**——它改写磁盘上的资产，
    而资产编辑不在撤销栈范围内（与 Unity 一致）。

**未验证**：编辑器里真的拖一个 Prefab 进场景、在 Inspector 上改值后 Apply、保存场景再打开（UI 链路本身是第 3 条，未接）。`Level::load(url)` 带物理场景的完整路径未在无头环境跑过（它与 `LoadObjectGraph` 的差别只在建物理场景）。**Inspector 的 ZSlate 渲染**（结构行的按钮、can_apply 的显隐）只过了 `-fsyntax-only`，没有上机看。


---

## 附录 A：Unity 调研要点对照表（仅供参考）

| Unity 概念 | 实现位置（Unity 源码） | ZEngine 对应 |
| --- | --- | --- |
| `Prefab` (旧) | Runtime/BaseClasses/PrefabBackwardsCompatibility.h | 不实现，纯历史包袱 |
| `PrefabInstance` | Editor/Src/Prefabs/PrefabInstance.h | `Editor/Prefab/PrefabInstance` |
| `PropertyModification` | Editor/Src/Prefabs/PropertyModification.h | `Runtime/Resource/Prefab/PropertyModification`（2026-10-27 从 Editor 下沉，Variant 要进真机） |
| `PrefabUtility::InstantiatePrefab` | Editor/Src/Prefabs/PrefabUtility.cpp | `Runtime/Prefab/PrefabUtility::Instantiate` |
| `PrefabUtility::SaveAsPrefabAsset` | Editor/Src/Prefabs/PrefabUtility.cpp | `Editor/Prefab/PrefabSave.cpp` |
| `CorrespondingObjectFromSource` | EditorExtensionImpl::m_CorrespondingSourceObject | `PropertyModification.correspondingSource` + `PrefabInstantiateContext` |
| `m_PrefabAsset` | EditorExtensionImpl 字段 | `PrefabInstance.sourcePrefab` |
| Variant base 链 | PrefabAsset 自身字段 | `PrefabAsset.m_variant_base` + `m_variant_overrides`（Variant 不拥有对象树） |

> 本文档以 Unity 2023.1 在 `e:/Engine/unity2023.1` 的源码为参考；具体到 ZEngine 上的语义有调整（去除历史 API、合并 Editor-only/Runtime 边界、`.zasset` 二进制取代 YAML）。

**P16-P17 暴露并修掉的问题（2026-10-26）**：

16. **YAML 场景没有依赖**（见 §9.4 第 4 条）：AssetRegistry 的依赖索引只读二进制头。
17. **AssetRegistry 依赖边与扫描顺序有关**：扫描循环里每个文件一入表就解析它的外部引用，引用目录序更靠后的资产时查不到、边被丢弃。改为整轮登记完再建边（异步扫描与 `IncrementalUpdate` 两处）。
18. **零位置经 Apply / Revert / Merge 回不去**：覆盖按叶子逐个拼进字节流再读回，`Transform::Transfer` 的读取分支在双精度位置为零、旧 float 副本非零时回退到旧副本——而拼完最后一个叶子时旧副本还是上一次的值，结果位置变回原值。这条回退只对按名字查找的读取器有意义（旧数据里根本没有 `m_LocalPositionD` 字段），按位置读的 StreamedBinaryRead 拿到的总是写入器成对写出的两份，现在只在前者上回退。
