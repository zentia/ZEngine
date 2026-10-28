# P0.2 去掉动画采样路径的每帧整片拷贝：修改方案与验收标准

> 日期：2026-10-28 ｜ 状态：**方案稿，未动代码** ｜ 上游：`doc/animation/BLENDTREE_ROOTMOTION_ESTIMATE.md` §3 M0 / P0.2
> 行号以本文写作时的 `main` 为准。

> **落地记录（commit 1，2026-10-28）**：`AnimationPipelineSmokeTest` 已实现并通过（14/14，跑在旧代码上全绿）。基线 P7 哈希 `17764afb96eb6b4b`（pipeline 与 component 两条路径一致）。Debug bench 基线（60 骨 2 片段）：30 帧 142.7 µs / 120 帧 154.0 µs / 600 帧 217.9 µs —— 已能看出随帧数增长（验证 H1）。Release 未测（`bin/Release/ZRuntimeShared.dll` 陈旧 5.8 MB @10:07，需先整库重建 Release 才能跑真 A/B，留到 commit 3）。
>
> **⚠️ 测试过程中发现一个既有 bug（不在 P0.2 范围内，未修）**：`Skeleton::ApplyAnimation` 的 blend 累加器用 `BonePose::Identity()`（scale=1）作零值，`AccumulateBlend` 在 1 之上再累加各片段 scale，`NormalizeBlendAccumulator` 除以 total_weight（单片段=1）。位置起点是 0、旋转经归一化自校正，**唯独 scale 错**：单片段 scale 变成 2，N 片段变 N 倍。测试用 `P0` 检查把它钉住（见测试注释），保证 P0.2 重构不会悄悄改变它；将来修复应是有意的独立提交。此 bug 会导致蒙皮角色等比放大（子骨骼偏移 ×2/×4/×8 累积）。

## 0. 一句话

`AnimationComponent::Tick` 每帧经 `GetBlendStateWithClipData` 把**每个参与混合的 AnimationClip 整个深拷贝一次**（含全部通道名和关键帧数组）。改成持有指向缓存的 `const AnimationClip*`，顺带把这条路径上的其它逐帧堆分配收成调用方持有的 scratch。**正常数据下姿势逐位不变**；工作量 **1.5 – 2 人日**。

---

## 1. 现状：一次 `AnimationComponent::Tick` 在热路径上干了什么

每个带动画的对象、每帧、三条分支（Timeline 覆盖 / 状态机 / 旧 BlendState）都走同一套：

| # | 位置 | 行为 | 每帧代价（N = 片段数，B = 骨骼数，F = 帧数） |
|---|---|---|---|
| **H1** | `AnimationSystem.cpp:126` | `blend_clip.push_back(*found->second)` —— **深拷贝整个 AnimationClip** | 每片段：1 + 通道数×4 次分配，拷 B×F×40 B。60 骨 × 120 帧 ≈ **288 KB / 片段 / 帧**，≈ 240 次分配 |
| H2 | `AnimationSystem.cpp:100,104,108` | `for (auto path : ...)` 按值遍历 + `TryLoad*(eastl::string)` 按值传参 | 每路径 2 次字符串拷贝（超 SSO 即分配） |
| H3 | `AnimationSystem.cpp:100-136` | 每个路径先 `TryLoad*` 查一次 map，再 `find` 查一次 | 每路径 2 次 `std::map<eastl::string>` 查找 |
| H4 | `AnimationSystem.cpp:113,195-199` | 结果按值返回；`blend_weight.assign(clip_count, BoneBlendWeight{})` 销毁内层容量 | 每帧 4 个 vector + N 个 B 长 float vector 重新分配 |
| H5 | `Skeleton.cpp:84-85,105` | `accumulator` / `total_weight` / 每片段 `poses` 局部 vector | 每帧 2 + N 次分配 |
| H6 | `Skeleton.cpp:162-191` + `AnimationComponent.cpp:85/97/128` | `OutputAnimationResult()` 按值返回新 vector 再赋值 | 每帧 1 次分配 |

H1 的量级远大于其它各项之和，而且**和片段长度成正比** —— 这是本次的主目标。H2–H6 是同一路径上的机械修改，一起做掉，避免改两次。

### 1.1 本次顺手确认的不变量

- **缓存条目从不删除**：全仓对 `m_AnimationDataCache` 没有 `erase` / `clear`（已 grep）；`std::map` 节点地址稳定，`shared_ptr` 由 map 持有 ⇒ 进程内指针一直有效。
- **`BlendStateWithClipData` 从未被序列化**：它有 `DECLARE_SERIALIZE`，但全仓没有 `BlendStateWithClipData::Transfer` 的定义和实例化（已 grep）⇒ 改成员类型不影响任何资产格式。
- **`blend_clip` 的唯一读者是 `Skeleton::ApplyAnimation`**（`Skeleton.cpp:88,102`，已 grep）。

### 1.2 顺带发现的潜在 bug（不在本次主改动里）

`GetBlendStateWithClipData` 加载失败时用 `continue` 跳过（`:118-127` 片段、`:128-137` 骨骼映射），**但三张表各自压缩，没有按槽位对齐**：

```
clip_paths = [A(加载失败), B]     maps = [mA, mB]     ratio = [rA, rB]
⇒ blend_clip = [B]   blend_anim_skel_map = [mA, mB]   clip_count = 1
⇒ B 用 mA 的骨骼映射、rA 的相位、A 的遮罩采样
```

只在"某个片段丢失"时触发，表现为错骨骼乱动而不是报错。修它会改变出错路径的行为，所以单独作为 **commit 4（可选）**，不混进"逐位不变"的重构里。

---

## 2. 修改方案

### 2.1 数据结构（`Runtime/Resource/ResType/Data/BlendState.h`）

```cpp
/// Runtime-only, never serialized. Filled and consumed inside ONE
/// AnimationComponent::Tick; do not keep across frames (see AnimationSystem).
class BlendStateWithClipData
{
public:
    int clip_count {0};
    /// Non-owning. Points into AnimationSystem's clip cache, whose entries are
    /// never erased. Previously a by-value std::vector<AnimationClip>, which
    /// deep-copied every key of every blended clip every frame.
    std::vector<const AnimationClip*> blend_clip;
    std::vector<PPtr<AnimSkelMap>> blend_anim_skel_map;   // 不变
    std::vector<BoneBlendWeight> blend_weight;            // 不变（内层容量复用）
    std::vector<float> blend_ratio;                       // 不变
};
```

- 删掉 `DECLARE_SERIALIZE(BlendStateWithClipData)`：它本来就没有 Transfer，留着只会误导人以为它能存盘。
- `blend_anim_skel_map` 保持 `PPtr`（只是整数句柄，没有深拷贝；改成裸指针属于另一件事）。

### 2.2 `AnimationSystem`（`.h` / `.cpp`）

| 改动 | 说明 |
|---|---|
| 新签名 `static void BuildBlendStateWithClipData(const BlendState& in, BlendStateWithClipData& out);` | **删除**按值返回的旧函数（只有 `AnimationComponent` 三处调用），不留两条路径 |
| 入口第一件事 `out.clip_count = 0; out.blend_clip.clear(); out.blend_anim_skel_map.clear(); out.blend_ratio.clear();` | 复用 scratch 的铁律：先清再填（`MEMORY.md` GPU-Spine 节） |
| 片段：`const AnimationClip* clip = FindOrLoadClip(path); if (!clip) { LOG_ERROR; continue; } out.blend_clip.push_back(clip);` | 一次查找，拿指针；**H1 消失** |
| `TryLoadSkeleton / TryLoadAnimation / TryLoadAnimationSkeletonMap / TryLoadSkeletonMask` 参数改 `const eastl::string&`；`for (const auto& path : ...)` | H2 |
| 新增私有 `FindOrLoadClip` / `FindOrLoadSkelMap` / `FindOrLoadMask`：一次 `find`，未命中才 load + emplace | H3（预加载循环 `:100-111` 合并进填充循环） |
| `blend_weight`：`out.blend_weight.resize(clip_count); for (auto& w : out.blend_weight) w.blend_weight.assign(bone_count, 0.0f);` | 不再 `assign(…, BoneBlendWeight{})`，内层容量保留 —— H4 |
| 权重归一化循环（`:201-235`）**一行不改** | 保证数值逐位一致 |

### 2.3 `Skeleton`（`.h` / `.cpp`）

```cpp
// Skeleton.h 新增成员（ApplyAnimation 本来就非 const）
std::vector<AnimationSkinning::BonePose> m_AccumulatorScratch;
std::vector<float>                       m_TotalWeightScratch;
std::vector<AnimationSkinning::BonePose> m_PoseScratch;
```

- `ApplyAnimation`：`assign(bone_count, …)` 到成员 scratch（`assign` 在容量足够时不分配）；`SampleClip` 本来就是 `out_poses.assign(...)`，传 `m_PoseScratch` 即可。读片段改成 `const AnimationClip& clip = *blend_state.blend_clip[i];`（指针在 `Build…` 里已判空，这里加一条 `if (!ptr) continue;` 兜底）。
- `OutputAnimationResult` 改为 `void OutputAnimationResult(AnimationResult& out)`：`out.node.clear(); out.node.reserve(bone_count);` —— H6。
- **混合数学（`AccumulateBlend` / `NormalizeBlendAccumulator` / `ComposeDelta`）一行不改。**

### 2.4 `AnimationComponent`

- 新增成员 `BlendStateWithClipData m_ClipDataScratch;`。三处 `const auto& clip_data = AnimationSystem::GetBlendStateWithClipData(x);` 改为 `AnimationSystem::BuildBlendStateWithClipData(x, m_ClipDataScratch);`，`m_Skeleton.OutputAnimationResult(m_AnimationRes.animation_result);`。
- `m_ClipDataScratch` 不加 `ZPROPERTY`（不序列化、不进 Inspector）。

### 2.5 明确**不**在本次做的

| 项 | 原因 / 去处 |
|---|---|
| `AnimatorInstance` 初始化时把路径预解析成指针，帧内不再查 map | 要改 `AnimatorInstance` 的数据结构，和 BlendTree 子项结构一起设计（估算文档 B4） |
| `PPtr<AnimSkelMap>` 逐帧解引用的 ObjectManager 查找 | 量级远小于 H1；单独量过再说 |
| `std::map` → `unordered_map` | 同上 |
| 缓存失效 / 资产热重载 | 现在根本没有失效机制；以后加时见 §4 的生命周期约束 |

---

## 3. 提交拆分

| commit | 内容 | 关键点 |
|---|---|---|
| **1** | 新测试 `AnimationPipelineSmokeTest`（只加测试，**跑在旧代码上必须全绿**）+ CMake + CI | 先有尺子再动刀；commit 1 的二进制就是 A/B 的 A 侧 |
| **2** | §2.1 + §2.2 中"指针化 + 去字符串拷贝 + 单次查找" | 解决 H1–H3 |
| **3** | §2.2 out 参数 + §2.3 + §2.4 scratch 化 | 解决 H4–H6 |
| 4（可选） | §1.2 槽位对齐修复 + 对应测试用例 | 只改出错路径的行为，单独评审 |

提交按 AGENTS 约定**点名 `git add`**，不用 `-A`。

---

## 4. 风险与生命周期约束

| 风险 | 对策 |
|---|---|
| 裸指针悬空 | 指针只在**一次 Tick 内**被填充并消费（`Build…` → `ApplyAnimation`），跨帧不读旧值；缓存条目永不删除。两条都写进 `BlendStateWithClipData` 的注释。**以后谁加缓存失效，必须保证不在 Tick 中途失效** |
| scratch 残留上一帧数据 | 入口先 `clear()`；测试 P6 专门钉住"2 片段 → 1 片段"缩容 |
| 重构不小心改了数值 | 混合数学函数一行不动；测试 P7 用同一份夹具在 commit 1 与 commit 3 两个二进制上比对姿势哈希 |
| 与另一会话并发改 Animation 目录 | 动手前按 `MEMORY.md` 流程看 `git diff HEAD` / mtime |

---

## 5. 验收标准

### A. 正确性（硬门槛，全部必须过）

新测试 `engine/Source/Runtime/Function/Animation/Test/AnimationPipelineSmokeTest.cpp`，开关 `ZENGINE_BUILD_ANIMATION_PIPELINE_SMOKE_TEST`。走**真实**的 AssetManager：把合成的 `AnimationAsset` 与 `AnimSkelMap` 用 `WriteObjectToDiskThreadSafe` 写到临时目录，再经 `BuildBlendStateWithClipData` → `ApplyAnimation` → `OutputAnimationResult`（和 `AnimatorSmokeTest` A11 同一套路）。夹具：3 骨链，关键帧取浮点可精确表示的值（0 / 0.5 / 1 / 2，绕单轴 90° 等）。

| ID | 用例 | 判定 |
|---|---|---|
| P1 | 单片段，相位 0 / 0.5 / 1−ε | 调色板矩阵与解析值误差 ≤ 1e-5 |
| P2 | 两片段，权重 0.5 / 0.5 | 与解析平均值误差 ≤ 1e-5 |
| P3 | 带 `BoneBlendMask`（片段 A 只驱动骨 1，B 只驱动骨 2） | 每骨只受对应片段影响 |
| P4 | 一个片段路径不存在 | 不崩溃；该片段被跳过；行为与旧代码一致（commit 4 前不修 §1.2） |
| **P5** | **指针同一性**：`clip_data.blend_clip[i] == AnimationSystem::TryLoadAnimation(path).get()` | 证明没有拷贝。commit 1 上这条用 `#if` 关掉（旧类型不是指针），commit 2 起打开 |
| P6 | scratch 复用：先 2 片段 Build，再 1 片段 Build | `clip_count == 1`，各数组 size 都是 1，无残留 |
| **P7** | **A/B 姿势哈希**：`AnimatorInstance` 驱动 idle→run 交叉淡化 + 循环回绕，跑 200 帧（dt = 1/60），对每帧全部调色板 float 的**位模式**做 FNV-1a 64，打印一个哈希 | **commit 1 二进制与 commit 3 二进制打印的哈希必须完全相同**（同编译器同配置，逐位比较是合法的） |

外加：现有 `AnimationSkinningSmokeTest`（19 个 Check）与 `AnimatorSmokeTest`（37 个 Check）全绿。

### B. 结构（硬门槛）

| ID | 判定 |
|---|---|
| B1 | `BlendStateWithClipData::blend_clip` 的元素类型是 `const AnimationClip*`（测试里加 `static_assert`） |
| B2 | 运行时代码里不再有对 `AnimationClip` 的按值拷贝：`grep -n "push_back(\*found->second)"` 为空，`std::vector<AnimationClip>` 只剩资产侧定义 |
| B3 | 稳态无扩容：预热 10 帧后再跑 1000 帧，`m_ClipDataScratch` 各 vector 及每个 `blend_weight[i].blend_weight` 的 `capacity()` 不变（测试持有自己的 `BlendStateWithClipData` 来查） |

> 为什么不直接数堆分配：这个测试必须链 `ZRuntime`（要 AssetManager），而重载全局 `new` 的 exe 不能链 `ZRuntime`（LNK2005，见 `MEMORY.md`）；MSVC Debug 下容器还有 `_Container_proxy` 假阳性。所以用 **P5 指针同一性 + B3 容量不变**这两个结构性证据代替分配计数。

### C. 性能（Release，`--bench` 参数触发，不进 ctest）

夹具：60 骨、2 片段、每次迭代 = 一次 `Build…` + `ApplyAnimation` + `OutputAnimationResult`，每组 10 000 次，**commit 1 与 commit 3 两个二进制交错各跑 3 轮取中位数**（绝对值会漂，只看比值 —— `MEMORY.md` 量化方法论）。

| ID | 判定 | 说明 |
|---|---|---|
| **C1** | **改后每次迭代耗时与片段长度无关**：30 帧片段 vs 600 帧片段，耗时差 ≤ 15% | 最有说服力的一条：H1 存在时成本与帧数成正比（同时记录旧二进制上的这组数，预计接近 20 倍差） |
| C2 | 120 帧片段下，新旧耗时比 ≤ 0.5（至少快 2 倍） | 硬门槛只定 2 倍；按 §1 的量级粗估应在 3–8 倍，**这是估计不是实测**，以实测为准写回本文 |

### D. 实机回归

| ID | 判定 |
|---|---|
| D1 | ZEditor（`-p E:/Engine/ZEngine/engine/ZGameSmoke.zproject`，启动关卡 `asset/level/1-1`；`1-1.level.json` 引用了 `player.object.json`，后者用 `W2_CrouchWalk_Aim_F_Loop_IP`。**动工前先确认 `1-1.scene` 里该角色确实带 AnimationComponent**，没有的话手动放一个）进 PIE：动作目视与改前一致；用 `--cvar "exec.after 600 quit"` 无人值守跑一遍，日志 `[E]` 条数与改前相同、没有新增 `ZAnimation` 报错 |
| D2 | `bin/Debug/crash_stack.txt` 不新增 |
| D3 | 若 `build-web` 目标包含 Animation 模块，WASM 编译通过 |

### E. CI

| ID | 判定 |
|---|---|
| E1 | `engine/Source/Runtime/CMakeLists.txt` 加两条 `list(REMOVE_ITEM … AnimationPipelineSmokeTest.cpp)`（否则 LNK2005） |
| E2 | `.github/workflows/smoke_tests.yml` 的 Configure 加 `-DZENGINE_BUILD_ANIMATION_PIPELINE_SMOKE_TEST=ON`，Build target 列表加 `AnimationPipelineSmokeTest` |
| E3 | 本地 ctest 跑三个动画测试全绿 |

---

## 6. 工作量

| 项 | 人日 |
|---|---|
| commit 1：测试夹具 + P1–P7 + CMake/CI | 0.5 – 0.75 |
| commit 2：指针化、去字符串拷贝 | 0.25 |
| commit 3：scratch 化（AnimationSystem / Skeleton / AnimationComponent） | 0.25 – 0.5 |
| Release 基准（C1/C2）+ 实机回归（D） | 0.25 – 0.5 |
| **合计** | **1.25 – 2** |
| commit 4（可选，槽位对齐修复 + 测试） | + 0.25 |
