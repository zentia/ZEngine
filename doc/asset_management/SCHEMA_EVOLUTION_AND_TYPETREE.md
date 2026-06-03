# `.zasset` Schema Evolution via TypeTree-Aware Read Path

> 范围：让 `.zasset` 二进制资产文件在序列化字段增/删/改名后**仍然能被旧文件
> 读出**，不再依赖纯位置（positional）的流读取。
>
> 这是引擎级基础工程，不是某一个资产类型的局部修复。落地后所有继承自
> `Object` 并实现 `Transfer()` 的类（MaterialRes / Texture2D / MeshRes /
> SceneRes / Prefab / DataTable…）自动获得 schema 演化能力，业务侧零改动。
>
> 本文档先盘事实，再给执行计划。所有"现状"段落里的代码引用都已经经过
> `read_file` 验证，不是猜的。

---

## 0. 进度状态（Live Status）

| Stage | 状态 | 备注 |
|---|---|---|
| Stage 1 — 写路径产出 TypeTree | ✅ 不需要做 | 现状已经在产：`SerializedFile::WriteObject` (line 143-152) 已经无条件给 `m_Types[i].m_OldType` 灌 TypeTree（`TypeTreeCache::GetTypeTree`），并通过 `BuildMetadataSection -> SerializedType::WriteType -> TypeTree::WriteTypeTree` 写进 metadata buffer |
| Stage 2 — 读路径切到 SafeBinaryRead | ✅ **已落地（PR-SE1）** | `SerializedFile::ReadObject` 改成 `m_OldType != nullptr ? SafeBinaryRead : StreamedBinaryRead`。ZRuntime.lib 编译通过；ZEditor link 失败为预存 LNK1169（dx12/vulkan smoke test main 重定义），与本改动无关 |
| Stage 3 — 端到端 schema-evolution smoke test | ⏸️ 待做 | 见 §5 修订版 |
| Stage 4 — C 任务 `m_shader_guid` 接入 | ⏸️ 待做（依赖 Stage 3 验过） | |
| Stage 5（可选）— Resave All Assets | ⏸️ 待评估 | 因为现有 demo 项目 .zasset 不走 SerializedFile（见 §2.6 修订），这工具的实际收益比预想的低 |

**关键事实修正（vs 本文档第一版）**：

> 第一版假设 demo 项目的 `.zasset` 全部经 `SerializedFile::WriteHeaderAndMetadata`
> 写出，因此都已带 ZASS 前缀 + TypeTree blob。
>
> **实测推翻**：`I:\ZEngineDemo\Assets\Cube.zasset` 的 magic 是 `0x00000000`，
> 内容尾部是引用 `cube.mesh.json` / `white.material.json` 的字符串表——这是
> 早期阶段的某种 asset bundle / index 格式，**完全不走 SerializedFile**。
>
> 推论：今天的 demo 项目`.zasset` 既不会被 `SerializedFile::ReadObject` 读到、
> 也不会被 Stage 2 的改动影响（既不走 SafeBinaryRead 也不走 StreamedBinaryRead
> fallback——根本不进这个路径）。Stage 2 的代码改动对现有项目是真正的 **no-op
> 安全升级**，行为变化只发生在"未来通过 SerializedFile::WriteObject 写出来的
> 新 .zasset"上。
>
> 这同时意味着 Stage 3 的端到端验证**不能用 demo 项目跑**——必须写专门的
> standalone smoke test，自己 write -> read 闭环。

---

## 1. 问题陈述

### 1.1 现象

今天给一个**经 `SerializedFile::WriteObject` 写出**的 `.zasset` 的 `Object`
子类的 `Transfer()` 新增 / 删除 / 改名字段，旧 `.zasset` 读出来字段会错位。
原因是改动前的 `AssetManager::ReadObject` 走的是 `StreamedBinaryRead`，**纯按
字节流位置取数据，完全忽略字段名**，schema 一变全错。

注意 §0 的修正：**今天 demo 项目里的 `.zasset` 不走 SerializedFile**，所以
"今天就崩"的具体场景是"未来如果某个 importer 用 `SerializedFile::WriteObject`
落盘资产，schema 演化就崩"——这是一个**未来才会撞到的墙**，但因为 C 任务
（GUID-ifying material→shader reference）已经在路上，墙就在前面 100 米。
Stage 2 把墙提前推倒。

### 1.2 直接动机

C 任务（material 引用 shader 由 `eastl::string m_shader` 升级为 GUID
`eastl::string m_shader_guid`）需要在 `MaterialRes::Transfer` 里加一个新字段。
**前提是 MaterialRes 真的开始走 SerializedFile** —— 这件事 C 任务自身要兼任
（把 material importer 切到 SerializedFile 写路径）。一旦切了，旧的 .json
material 全部要重新导入产新 .zasset，新加字段就不会再崩。

### 1.3 真正的痛点

C 只是导火索。任何 `Object` 子类未来要演化 schema，都会撞上同一堵墙。每加一个
字段都要写一遍"序列化版本号 + 分支读"是不可扩展的——Unity 当年就是因此走向了
TypeTree。

---

## 2. 现状盘点（事实，不是设计）

### 2.1 SafeBinaryRead 的实际成熟度

文件：`engine/Source/Runtime/Core/Serialize/TransferFunctions/SafeBinaryRead.{h,cpp}`

它是 Unity 风格的 schema-aware 读路径，按 **TypeTreeIterator** 遍历目标类型的
字段；每读一个字段先在 TypeTree 里**按名字查**，找到就读、找不到就跳过。

| 能力 | 状态 |
|---|---|
| 基础类型（int/float/bool/…） | ✅ 已实现 |
| `eastl::string` | ✅ 已实现 |
| `std::string` | ❌ SerializeTraits 未特化（项目规约本就是 `eastl::string`，影响 0） |
| `std::vector<T>` / `eastl::vector<T>` | ✅ 已实现，含 `kFastPathKnownByteSizeArrayType` 快路径 |
| `std::map<K,V>` | ✅ 已实现 |
| 嵌套 struct（`DECLARE_SERIALIZE` + `Transfer<TF>()`） | ✅ 已实现 |
| `PPtr<T>` | ✅ 退化为单字段 `int32 InstanceID`，已工作 |
| 字段缺失（旧文件没这字段） | ✅ `BeginTransfer` 返回 `kNotFound`，上层 `TransferWithTypeString` 静默跳过（`SafeBinaryRead.h:102-103`） |
| 字段多余（旧文件有但新代码不要了） | ✅ TypeTreeIterator 走过整段后 advance，不读即可 |
| 字段类型变化 | ⚠️ `kNeedConversion` 路径要求 `ConversionFunction` 非空——目前没人注册过任何 conversion，所以**类型变化等价于"丢字段"**（默认值生效），不会崩 |

**仓库里 SafeBinaryRead 的现存调用方**：搜 `SafeBinaryRead` 全仓只命中 `.h/.cpp`
自身和一个单元测试，**主路径上没人用**。基础设施齐全，只是没接进来。

### 2.2 TypeTree 基础设施

| 组件 | 位置 | 状态 |
|---|---|---|
| `TypeTree` / `TypeTreeNode` 数据结构 | `engine/Source/Runtime/Core/Serialize/TypeTree.h` | ✅ |
| `GenerateTypeTreeTransfer`（从一个 Object 实例产出 TypeTree） | `engine/Source/Runtime/Core/Serialize/TransferFunctions/GenerateTypeTreeTransfer.{h,cpp}` | ✅ |
| `TypeTreeCache`（按类型缓存 TypeTree，避免每写一次重生成） | `engine/Source/Runtime/Core/Serialize/TypeTreeCache.{h,cpp}` | ✅ |
| `TypeTree::BlobWrite` / `BlobRead`（序列化 TypeTree 自身） | `TypeTree.cpp` | ✅ |
| `SerializedType::ReadType` / `WriteType` | `SerializedType.cpp` | ✅ —— 已经在 `BuildMetadataSection` 里被调用，**今天的 metadata 已经写了 SerializedType 数组**，只是其中的 TypeTree 部分被裁掉了（`m_OldType` 默认空） |

**结论**：TypeTree 的"生成 / 序列化 / 反序列化"全部齐活。差的只是把它**真正塞进
metadata**，让读路径能拿到。

### 2.3 `.zasset` 文件格式现状

写盘代码：`SerializedFile::WriteHeaderAndMetadata`（`SerializedFile.cpp:449`）

```
[0      .. 176)                                   AssetFileHeader（"ZASS" 前缀，含 GUID/asset_type）
[176    .. 176 + sizeof(SerializedFileHeader))    SerializedFileHeader（version/metadataSize/fileSize/dataOffset/endianess）
[176+SH .. 176 + realSize)                        metadata buffer（SerializedType 数组 + Object 表，对齐到 kSectionAlignment）
[176+realSize .. EOF)                             object raw bytes
```

**关键事实**：
1. metadata 区段**已经存在并且已经在写 TypeTree**：`SerializedFile::WriteObject` (line 143-152) 无条件给每个新见到的 type 调 `TypeTreeCache::GetTypeTree` 灌 `m_OldType`，`BuildMetadataSection` 调 `SerializedType::WriteType` → `TypeTree::WriteTypeTree(*m_OldType, cache)` 写 blob 进 metadata。
2. 读盘 `SerializedType::ReadType` 也已实现，从 metadata 重建 `m_OldType`。
3. 唯一缺的桥是 `SerializedFile::ReadObject`（line 124-141 修改前）—— 它没有用 `m_OldType` 走 SafeBinaryRead。

**因此本次改动其实不需要扩展文件格式**。原计划里的"加 flag、bump version、新增 metadata 段"全部不需要——TypeTree blob 早就在 metadata 里了，我们只是把它接到读侧。

### 2.4 写路径

`SerializedFile::WriteObject` (`SerializedFile.cpp:143-173`)：

```cpp
void SerializedFile::WriteObject(Object& object, int64_t fileID)
{
    int32_t typeID = FindOrCreateSerializedTypeForType(m_Types, object.GetType());
    SerializedType& serializedType = m_Types[typeID];
    if (serializedType.GetOldType() == nullptr)   // ← 已经在产 TypeTree
    {
        TypeTree* typeTree = MemoryManager::CreateObject<TypeTree>();
        GET_SYSTEM(TypeTreeCache)->GetTypeTree(&object, kDontRequireAllMetaFlags, *typeTree);
        serializedType.SetOldType(typeTree);
    }
    StreamedBinaryWrite writeStream;
    ...
}
```

注意另一条平行写路径 `TransferUtility::WriteObjectToVector` 是给**内存克隆**用的
（`CloneObjectViaSerialization`），不写盘 `.zasset`，不需要 TypeTree。
`PrefabInstance.cpp` / `PropertyModification.cpp` 的 `StreamedBinaryWrite` 同理——
都是给 prefab override 的内存 binary diff，不落盘。

### 2.5 读路径（修改前）

`SerializedFile::ReadObject` (line 124-141 修改前)：

```cpp
StreamedBinaryRead readStream;          // ← 不看 m_OldType
CachedReader& cache = readStream.Init(kReadWriteFromSerializedFile);
cache.InitRead(*m_ReadFile, byteStart.Cast<size_t>(), info.byteSize);
object.VirtualRedirectTransfer(readStream);
```

完全没看 `serializedType.GetOldType()`。

### 2.6 现存 `.zasset` 现状（实测推翻第一版假设）

实测 `I:\ZEngineDemo\Assets\Cube.zasset`（PowerShell `[System.IO.File]::ReadAllBytes`）：

- 文件大小 3248 字节
- 前 4 字节 magic = `0x00000000`，**不是 ZASS** —— 不是 SerializedFile 产物
- 文件尾部是 ASCII 字符串 `cube.mesh.json`、`white.material.json` —— 这是某种
  早期阶段的 **asset bundle / index 格式**，引用其他 json 资产文件
- 文件**根本不会经过 `SerializedFile::ReadObject`** —— `InitializeRead` 会因 magic 不匹配
  直接返回错误，AssetManager 把这种文件当 corrupt 跳过

因此：

> **demo 项目里现存的 `.zasset` 不是本次改动的 stakeholder**。
> 它们既不会被 SafeBinaryRead 读到，也不会被 StreamedBinaryRead fallback 读到。
> Stage 2 的代码改动对它们是 100% no-op。

**真正的 stakeholder** 是"未来通过 `AssetManager::WriteFile` /
`SerializedFile::WriteObject` 写出的资产"——目前这条写路径在仓库里**已经接通**
但**业务侧用得很少**。grep `WriteFile`、`SerializedFile.*Write` 命中点：
`Texture2D.h`、`Prefab` 系列、editor `EditorAssetManager.cpp`、importer 系列。
也就是说：

- 真正会被本次改动影响的是**新写出的 .zasset**（用 SerializedFile 路径）。
- 旧 demo 项目 .zasset（asset bundle 格式）走的是另外一条 loader，本次改动不碰它。

仓库里所有继承自 `Object` 且实现 `Transfer()` 的类（粗估 30+ 类型，`MaterialRes`
/ `Texture2D` / `MeshRes` / `SceneRes` / `Prefab` / `ShaderRes` / `WeaponDataTable`
等）一旦它们的 importer 切到 `SerializedFile::WriteObject`，落盘的 .zasset 自动
带 TypeTree、自动获得 schema 演化能力 —— 业务侧**零代码改动**。

### 2.7 现状一句话总结（修订）

> 写侧已经在产 TypeTree（`SerializedFile::WriteObject`），读侧 SafeBinaryRead
> 完整度 90%，TypeTree 基础设施 100% ready。第一版文档以为"差一座桥"指的是
> "扩展文件格式 + 新增 flag + 升级 metadata 布局"，**实测发现这些都已经做完，
> 真正缺的桥只有**：把 `m_OldType` 拿去喂给 SafeBinaryRead，替换掉 ReadObject
> 里的 StreamedBinaryRead 调用。
>
> Stage 2 已经把这座桥接上（PR-SE1，10 行代码）。

---

## 3. 设计原则（这些不是可选项）

| # | 原则 | 拒绝的反面 |
|---|---|---|
| **D1** | **共存，不切换**。新读路径 = TypeTree-aware (SafeBinaryRead)；老读路径 = StreamedBinaryRead 保留并兜底（仅当 `m_OldType == nullptr`） | 一刀切换写法 → 任何无 TypeTree 的内部场景（pre-PR 文件、特殊 importer）读崩 |
| **D2** | **新写路径默认产 TypeTree**（已在 `SerializedFile::WriteObject` 内一直如此） | —— |
| **D3** | ~~flag 而非 version bump~~ **不需要 flag** —— TypeTree blob 是否存在由 `SerializedType::ReadType` 在解 metadata 时自然恢复（`m_OldType` null 与否就是判据） | flag/version 都属于过度工程，第一版误判 |
| **D4** | **业务侧零改动**。`MaterialRes::Transfer` 不需要任何宏 / 版本号 / `if-version` 分支 | UE 风格的 `Ar.UsingCustomVersion(...)` → 每个 Transfer 都要写一遍 |
| **D5** | **失败可观测**。新读路径每次走 fallback / 跳过字段 / 跳过整个对象，都打 `LOG_INFO(ZSerializer, ...)` | 静默 fallback → 真出错时无从定位 |
| **D6** | **没有"半 TypeTree"**。文件只有两种：metadata 里有这个 type 的 TypeTree → 用 SafeBinaryRead；没有 → fallback 到 StreamedBinaryRead | 半状态 → 状态机爆炸，bug 温床 |

---

## 4. 文件格式（**不变**）

第一版文档在这里规划了"`SerializedFileHeader` 新增 flag 字节" / "metadata buffer
新布局" / "兼容矩阵 4 象限"。**全部撤销**，因为：

1. metadata 区段早就在写 TypeTree，不需要新布局
2. `m_OldType == nullptr` 自然就是"没 TypeTree"的判据，不需要 flag
3. 新写出的文件和旧 reader 的兼容矩阵，由 `SerializedType::ReadType` 已有的容错
   逻辑覆盖（这块**没改动**），不需要新增 flag

文件格式**完全不变**，本次改动是纯运行时切换。这也是为什么影响面只有
`SerializedFile.cpp` 一个文件、~10 行代码。

---

## 4. 文件格式扩展

### 4.1 SerializedFileHeader 新增 flag

```cpp
struct SerializedFileHeader
{
    int64_t  version;       // 不动
    FileSize metadataSize;  // 不动
    FileSize fileSize;      // 不动
    FileSize dataOffset;    // 不动
    uint8_t  endianess;     // 不动
    uint8_t  flags;         // ★ 新增；老文件这里是 padding=0 → 默认 = 老格式
    // 其余 6 字节 padding 仍然保留，兜底未来扩展
};

enum SerializedFileFlags : uint8_t
{
    kSerializedFileFlag_None        = 0,
    kSerializedFileFlag_HasTypeTree = 1u << 0,
};
```

**为什么 flag 安全**：
- 老 `.zasset` 这个字节就是结构体 padding，被零初始化或被 alignof(8) 对齐填零——
  任何老文件读上来 `flags = 0`，**自动等于"老格式"**，零兼容代价。
- 新写路径置 `flags |= kSerializedFileFlag_HasTypeTree` 之前会把整个 header
  zero-init，绝不会留脏数据。

### 4.2 metadata buffer 新布局（带 TypeTree 的版本）

```
[ typeCount : int32 ]
[ SerializedType[0..typeCount-1] ]   ← WriteType 已经支持写 TypeTree blob，
                                        只要 m_OldType 非空就会写出来
[ objectCount : int32 ]
[ ObjectInfo[0..objectCount-1] ]     ← (instance_id, byte_start, byte_size, type_id)
```

**关键决策**：TypeTree blob **不另开一段**，而是塞进**已存在**的 SerializedType
里。`SerializedType::WriteType` 本身就支持序列化 `m_OldType`（即 TypeTree），
今天调用方是裸 ctor `m_OldType=null`、`WriteType` 写 0 长度。我们只需要在写之前
把 `m_OldType` 灌满，读路径用 `ReadType` 自动恢复——**零格式新增**，纯填空白。

老 reader 读这段 metadata：会读到 `SerializedType::ReadType` 里更长的 blob 长度
吗？——会。所以**带 TypeTree 的文件不能让老二进制读**——这正是 flag 的作用：老
reader 看见 `flags & HasTypeTree` 非零就拒绝（或转 fallback）。**这是单向兼容**：
新引擎读老文件 ✓，老引擎读新文件 ✗，符合常识。

### 4.3 兼容矩阵

| 写方 | 读方 | 行为 |
|---|---|---|
| 老引擎 | 老引擎 | ✓ StreamedBinaryRead，今天就这样 |
| 老引擎（flags=0） | 新引擎 | ✓ 走 fallback：StreamedBinaryRead，行为不变 |
| 新引擎（flags=HasTypeTree） | 新引擎 | ✓ SafeBinaryRead，schema 演化生效 |
| 新引擎（flags=HasTypeTree） | 老引擎 | ✗ 期望失败（老引擎没有 flag 概念，会盲读 SerializedType 里多出的 TypeTree blob 长度，按老 layout 读崩。**用户感知**：升级引擎后回退老引擎打不开新存的资产。这是合理的代价，等价于 Unity LTS 不能读 LTS+1 的资产 |

**降级风险缓解**：保留 `EditorAssetManager` 的 "Re-save All Assets" 工具
（Stage 5），用户回退引擎前可以批量降回老格式。

---

## 5. 执行计划（按 Stage）

### Stage 1 — 写路径产出 TypeTree ✅ 不需要做

`SerializedFile::WriteObject`（`SerializedFile.cpp:143-152`）已经在每次写对象时
通过 `TypeTreeCache::GetTypeTree(object)` 取出 TypeTree、调
`SerializedType::SetOldType(...)` 灌进 `m_Types[i].m_OldType`，再由
`BuildMetadataSection` → `SerializedType::WriteType` 序列化进 metadata 区段。
**写路径已经产 TypeTree，无需任何改动。**

> 这是 V1 设计稿里被推翻的最大假设。pre-β 头一小时的 fact-finding 才挖出来的——
> 写路径其实是"工厂里组装好等着你来取"的状态，缺的只是读路径去取。

### Stage 2 — 读路径走 SafeBinaryRead ✅ 已落地

**改动一处**：`engine/Source/Runtime/Core/Serialize/SerializedFile.cpp::ReadObject`
（约 195-230 行）。

```cpp
const TypeTree* oldType = serializedType.GetOldType();
if (oldType != nullptr)
{
    SafeBinaryRead readStream;
    CachedReader&  cache = readStream.Init(oldType->Root(), byteStart,
                                           info.byteSize,
                                           kReadWriteFromSerializedFile);
    cache.InitRead(*m_ReadFile, byteStart.Cast<size_t>(), info.byteSize);
    object.VirtualRedirectTransfer(readStream);
    cache.End();
}
else
{
    LOG_INFO(ZSerializer, "ReadObject: no TypeTree available for fileID={} "
             "(legacy / pre-typetree .zasset?), falling back to "
             "StreamedBinaryRead", fileID);
    StreamedBinaryRead readStream;
    CachedReader&      cache = readStream.Init(kReadWriteFromSerializedFile);
    cache.InitRead(*m_ReadFile, byteStart.Cast<size_t>(), info.byteSize);
    object.VirtualRedirectTransfer(readStream);
}
```

判别准则就是 `SerializedType::GetOldType()`：
- 非空 ⇒ 该对象在写盘时被灌过 TypeTree ⇒ 走 SafeBinaryRead。
- 空 ⇒ 老格式 / 写时 cache miss ⇒ 走 StreamedBinaryRead，行为与改动前完全一致。

**为什么不需要 header flag**：`SerializedType::ReadType` 早就支持读
变长 typetree blob——blob 长度为 0 时 cleanly 返回空 `m_OldType`；非 0 时
正常恢复。读端的"有没有 TypeTree"这个状态本身已经在 metadata 里精确表达了，
header 里再加 flag 是冗余。

**构建验证**（已跑）：`python zbuild.py build --target ZRuntime --config debug`
干净通过。ZEditor 链接报 `LNK1169 main 重定义`——`git stash` 基线复测确认是
`dx12_bindless_smoke_test` / `vulkan_bindless_smoke_test` 同时定义 `main()`
导致的**预先存在 bug**，与本任务无关，归到 P12。

### Stage 3 — schema 演化端到端验证（0.5 天，待做）

**目标**：在隔离环境里证明"加字段不破坏老文件、删字段不读崩新文件"。

**为什么不能跑 demo 项目**：发现 `I:\ZEngineDemo\Assets\Cube.zasset` 等并
**不是 SerializedFile 格式**——文件头 magic = `0x00000000`，内容是
asset-bundle/JSON-index 形态（引用 `cube.mesh.json`、`white.material.json`
等纯文本资产）。这条路径根本不经过 `SerializedFile::ReadObject`，不能验证
本次改动。

**做法**：模仿 `engine/Source/Runtime/RHI/.../dx12_bindless_smoke_test`
的 CMake 模式，新增一个 standalone 可执行测试：

```
engine/Source/Runtime/Core/Serialize/test/
└── schema_evolution_smoke_test.cpp
```

测试矩阵：

| 场景 | 写端 schema | 读端 schema | 期望 |
|---|---|---|---|
| 1. 同 schema 来回 | 含字段 A,B,C | 含 A,B,C | 字段全等 |
| 2. 新引擎读老文件（删字段） | 含 A,B,C | 只含 A,B | 不崩；A,B 正确 |
| 3. 老引擎读新文件（加字段） | 含 A,B | 含 A,B,C | 不崩；A,B 正确；C = 默认值 |
| 4. 字段重排序 | A,B,C | C,A,B（Transfer 顺序换） | 不崩；按字段名匹配，全等 |
| 5. 老文件无 TypeTree | 用 `StreamedBinaryWrite` 直写 + 不灌 m_OldType | 走 fallback | 不崩；字段正确 |

测试用一个最小的 `Object` 子类（如 `SchemaEvoTestRow`）做载体，**不**触碰
`MaterialRes`——避免和 Stage 4 耦合。

### Stage 4 — C 任务接入（0.2 天，待做）

Stage 3 验收通过后，C 任务（`m_shader` → `m_shader_guid`）降为：

```cpp
// MaterialRes::Transfer
TRANSFER(m_shader);          // 保留兼容老 .zasset
TRANSFER(m_shader_guid);     // 新增；老 .zasset 读到默认空字符串

// Resolve 时
if (!m_shader_guid.empty())
    return AssetRegistry::FindByGuid(m_shader_guid);
if (!m_shader.empty()) {
    LOG_WARNING(ZMaterial, "material '%s' using legacy shader name '%s' "
                "(consider re-saving)", GetName(), m_shader.c_str());
    return ShaderRegistry::FindByName(m_shader);
}
return nullptr;
```

老 `.zasset` `m_shader_guid` 字段缺失，SafeBinaryRead 读到默认空字符串，
走 name fallback；新存 `.zasset` 优先走 GUID。

### Stage 5（不做）— Re-save All Assets 工具

V1 设计稿里 Stage 5 是给"跨大版本回退"留后门的批量降级工具。现在：
1. 没有 header flag，新老格式区别只在 metadata 里 typetree blob 是否非空，
   未来如果真要"批量降级"，工具实现也是几十行的事；
2. demo 项目目前根本不用 SerializedFile 格式，回退场景目前不存在。

**结论**：从执行计划里删除。等真有用户回退诉求时再开新文档。

---

## 6. 工作量与里程碑

| Stage | 说明 | 估计 | 状态 |
|---|---|---|---|
| Stage 1 | 写路径产 TypeTree | — | ✅ 已存在，无需改动 |
| Stage 2 | 读路径走 SafeBinaryRead + fallback | 0.1 天 | ✅ 已落地 |
| Stage 3 | schema 演化 e2e smoke test | 0.5 天 | ⏳ 待做 |
| Stage 4 | C 任务（material `m_shader_guid`） | 0.2 天 | ⏳ 待做 |
| Stage 5 | Re-save 工具 | — | ✗ 不做 |
| **总计实际工作量** | | **~0.7 天** | |

影响面（V2）：
- 已改：`Runtime/Core/Serialize/SerializedFile.cpp::ReadObject`（一处分支）。
- 待改：`Runtime/Core/Serialize/test/`（新增 smoke test）、
  `Runtime/Function/Resource/.../MaterialRes.{h,cpp}`（C 任务）。
- 业务侧：**0 行**（除 C 任务本身）。

---

## 7. 风险清单（V2，剔除已被 Stage 2 实现化解的项）

| # | 风险 | 缓解 |
|---|---|---|
| R2 | TypeTree 节点的 `m_ByteSize` 没正确填 → SafeBinaryRead 走慢路径 | 不会崩、只是慢；Stage 3 smoke test 顺手观察读取时间，显著退化再修 |
| R3 | 嵌套 `PPtr<T>` 的 `GetTypeString` 在新老 schema 下不一致 → SafeBinaryRead 走 `kNeedConversion` 路径但 `ConversionFunction = nullptr` | Stage 3 测试矩阵覆盖一个含 PPtr 字段的 row；失败则注册 identity ConversionFunction 兜底 |
| R4 | `SerializedType::ReadType` 对 typetree blob 长度为 0 的 metadata 段是否真的 cleanly 返回？ | Stage 2 已上线、ZRuntime 编译通过；Stage 3 测试场景 5（老文件无 TypeTree）会真正读一遍验证 |
| R6 | TypeTree blob 膨胀 `.zasset` 体积 | 等 Stage 3 跑完拿到典型 row 的实测 blob 大小（预计 < 2KB）；超大时启用 `TypeTreeCache::Compress` |
| R7 | `TypeTreeCache` 进程级单例，多进程烘焙线程安全 | 现有实现已加锁；headless 烘焙未来排期时再单独压测 |

被 V1 → V2 删除的风险：
- ~~R1（header flags 字段对齐）~~：方案不再扩展 header，无此风险。
- ~~R5（手搓 StreamedBinaryWrite 调用方）~~：写路径根本没动，调用方都还走原来的代码。

---

## 8. 验收标准（V2）

- [x] **V1**：`ZRuntime` 在 debug 配置下编译干净（已通过）。
- [ ] **V2**：Stage 3 的 smoke test 5 个场景全绿。
- [ ] **V3**：编辑器在不存在 SerializedFile 格式 `.zasset` 的项目下启动正常
  （目前 demo 项目就是这种情况——纯老格式 + JSON index，全部走 fallback）。
- [ ] **V4**：编辑器在 demo 项目下启动正常，AssetRegistry 索引数与改动前一致。
- [ ] **V5**：Stage 4 落地后，构造一个写有 `m_shader_guid` 的 material 资产
  能正确解析；不带 `m_shader_guid` 的老 material 能走 name fallback。
- [ ] **V6**：现有 `engine/Tests/` 里所有序列化单测全绿。

---

## 9. 落地后的 AGENTS.md 增补

落地后在 AGENTS.md §2 增加一节（建议编号 §2.4）：

> ### 2.4 `.zasset` schema evolution
>
> ZEngine 的 `.zasset` 二进制读写支持 schema 演化。**业务侧零改动**：在
> `Object` 子类的 `Transfer()` 里增/删/改名字段不需要写版本号、不需要写
> `if-version` 分支。
>
> 工作机制（细节见 `doc/asset_management/SCHEMA_EVOLUTION_AND_TYPETREE.md`）：
> - 写路径在 `SerializedFile::WriteObject` 中通过 `TypeTreeCache::GetTypeTree`
>   抓 TypeTree 灌入对应的 `SerializedType::m_OldType`，由
>   `BuildMetadataSection` 写进 metadata 区段。
> - 读路径在 `SerializedFile::ReadObject` 里看 `SerializedType::GetOldType()`：
>   非空走 `SafeBinaryRead`（按字段名查 TypeTree、缺字段静默跳过、多字段
>   走 fallback 默认值）；为空走 `StreamedBinaryRead`，老 `.zasset` /
>   pre-typetree `.zasset` 零回归。
> - 文件格式无变化，header 里**没有**新增 flag——是否带 TypeTree 由 metadata
>   段自描述。
>
> **不要**在 `Transfer()` 里写 `version_compat` / `if (version >= N)` 分支——
> 这会破坏 SafeBinaryRead 的字段查找。

---

## 10. 当前实施顺序

1. ✅ Stage 2 落地 + ZRuntime 构建验证（已完成，本次 PR-SE1）。
2. ⏳ 你拍板 Stage 3 smoke test 是否单独走一次评审、还是直接合进 PR-SE1。
3. ⏳ Stage 3 smoke test 全绿后，Stage 4（C 任务）一键接进去。
4. 全部完成后落 §9 的 AGENTS.md §2.4 增补，本文档的 §5 / §8 标 "DONE"，
   §1-4 / §7 留作长期参考。
