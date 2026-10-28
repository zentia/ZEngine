# BlendTree + Root Motion：从 0 到跑通的工作量评估

> 日期：2026-10-28 ｜ 状态：**评估稿，未动代码** ｜ 参考：Unity 2023.1 Mecanim（`../unity2023.1/Modules/Animation/mecanim/`）
> 所有"现状"结论均已对照当前 `main` 代码核实，行号以本文写作时为准。

## 0. 结论

| 里程碑 | 定义 | 人日（1 名熟悉本仓库的引擎程序） |
|---|---|---|
| **M0 前置** | 片段时长落盘 + 采样路径去掉逐帧整片拷贝 + 测试资产 | **2.5 – 4.5** |
| **M1 跑通（代码）** | 1D/2D BlendTree + Generic Root Motion（XY 平移 + Yaw）运行时完成；headless 冒烟测试进 ctest/CI；PIE 里一个角色 idle↔walk↔run 混合、一个 root motion 动作真位移 | **M0 + 12 – 18** |
| **M2 可用（编辑器）** | Animator 窗口能编辑 BlendTree 子项/阈值/参数；2D 混合空间预览；脚本 API 齐 | **M1 + 4 – 6.5** |
| **合计** | | **≈ 18.5 – 29 人日 ≈ 4 – 6 周** |

最小可行切片（只做 1D + 平移型 root motion，不做 2D / yaw / 编辑器 UI，控制器靠测试/脚本构造）：**≈ 11 – 17 人日**（M0 2.5–4.5 + 1D 树 4–5.5 + 平移 root motion 4.5–7）。

**最大的两个不确定项**：① Root bone 坐标系（FBX Y-up ↔ 引擎 Z-up、Armature 节点旋转）——±2 人日；② 测试资产——仓库里**只有一个原地动画**，没有任何带根位移的片段。

---

## 1. 现状盘点（决定了"从哪起步"）

### 1.1 已有、可直接复用的

| 能力 | 位置 | 对本任务的意义 |
|---|---|---|
| 状态机（参数 / 条件 / Trigger 消费 / exit time / 优先级 / 事件 / CrossFade） | `AnimatorController.h`、`AnimatorInstance.cpp`（553 行） | BlendTree 是"状态的 motion 从 1 个片段变成 N 个"，状态机本身不用重写 |
| **N 片段逐骨骼加权混合器** | `AnimationSystem::GetBlendStateWithClipData` + `Skeleton::ApplyAnimation`（`AccumulateBlend` / `NormalizeBlendAccumulator`） | 混合器本来就支持任意片段数、按骨骼归一化、四元数同半球 —— **BlendTree 不需要新采样器**，只需要产出更多 (clip, weight, phase) |
| 状态机冒烟测试 | `Function/Animation/Test/AnimatorSmokeTest.cpp`（37 个 Check，headless） | 新测试照抄骨架 |
| Animator 编辑器窗口 | `ZSlateAnimatorWindow.cpp`（2355 行，图画布 + 表格 + Clip/SkelMap 拾取器 + 拖拽） | BlendTree 编辑是在 `BuildStateBody` 上加子项表，拾取器可复用 |
| 脚本侧 `SetFloat/SetTrigger/CrossFade` | `AnimationComponent.h:49-88`（ZFUNCTION） | BlendTree 参数天然就能从 TS 驱动 |
| FBX 导入知道 fps 和时长 | `FbxAnimation.h:137-146` | 时长落盘只是"多写一个字段" |

### 1.2 缺口与隐患（按影响排序）

| # | 问题 | 证据 | 影响 |
|---|---|---|---|
| G1 | **每帧整片拷贝 AnimationClip** | `AnimationSystem.cpp:126` `blend_clip.push_back(*found->second)`，`AnimationComponent::Tick` 每帧调用 | 现在 1–2 个片段还能忍；BlendTree 5–9 个片段 ⇒ 每帧拷贝所有关键帧数组。**必须先修** |
| G2 | 采样路径每帧堆分配 | `Skeleton.cpp:84-85` accumulator/total_weight，`:105` poses（每片段一次） | 同上，片段数翻倍后放大 |
| G3 | **片段没有时长** | `AnimatorController.h:112-114`："`AnimationClip` only stores a frame count and the bake FPS is not persisted"；状态要手填 `clip_length` | BlendTree 每个子项都要时长才能做时间同步；手填 ×N 不可接受 |
| G4 | 没有 root bone 概念 | `SkeletonData::root_index` 被导入器写死成 0（`MeshImporter.cpp:577`） | Root motion 需要可配置的根位移骨骼 |
| G5 | 组件 Tick 顺序 = 挂载顺序 | `GameObject.cpp:56-68` 线性遍历 `m_Components` | Animation 若排在 Motor 后面，root motion 晚一帧或被覆盖 |
| G6 | 角色位移的唯一写入者是 Motor | `MotorComponent.cpp:141/271` `SetLocalPosition(m_TargetPosition)`；`Character.cpp:70-75` 另外写旋转 | Root motion 要接进 Motor，而不是另起一路写 Transform，否则互相覆盖 |
| G7 | `CharacterController::move` 只做"重叠就拒绝" | `CharacterController.cpp:55` `IsOverlap` → 原地不动 | Root motion 撞墙会卡死不滑；Jolt `PhysicsCharacter`（392 行）存在但**没接到任何组件**。本评估不包含修这个 |
| G8 | 测试资产 | 全仓 + `I:\ZEngineDemo` 只有 `W2_CrouchWalk_Aim_F_Loop_IP`（IP = in place，无根位移）一个片段 | M1 的 PIE 验证没素材 |

---

## 2. 设计要点（只写影响工作量的决策）

### 2.1 BlendTree

- **数据**：不新建资产类型，**扩展 `AnimatorState`**：`motion_type`（Clip / Blend1D / Blend2DSimpleDirectional / Blend2DFreeformCartesian / Direct）、`blend_parameter`、`blend_parameter_y`、`std::vector<BlendTreeChild> children`（clip 路径、skel map 路径、threshold、position_x/y、time_scale、cycle_offset）。旧控制器走 SafeBinaryRead 缺字段 = 默认 Clip，零迁移（`pos_x/pos_y` 已证明这条路可行，见 `AnimatorController.h:118-125`）。
- **首版不做**：嵌套树（子项再是树）、Mirror、FreeformDirectional（2D 方向型，+1 天，可后补）。
- **权重**：直接移植 Unity 算法（纯函数，好测）：
  - 1D：`blendtree.cpp:827 GetWeights1d`（相邻阈值线性插值，≈20 行）
  - 2D Cartesian：`blendtree.cpp:752`（gradient band，≈80 行）
  - 2D SimpleDirectional：`blendtree.cpp:500`（≈130 行）
  - Direct：`blendtree.cpp:834`（每个子项读自己的参数）
- **时间同步**：子项共享状态的 normalized time；状态有效时长 = Σ wᵢ·lenᵢ / Σwᵢ（Unity `blendtree.cpp:894/975`，`statemachine.cpp:622`）。`AdvanceState` 每帧用当前权重算有效时长，替代现在的 `state.clip_length`。
- **输出**：`RebuildBlendState` 产出 N 个片段（交叉淡化时 N+M 个，权重乘 `(1-fade)` / `fade`）；权重 < 1e-4 的子项直接不采样。
- **参数阻尼**：加 `SetFloat(name, value, damp_time, dt)`（Unity `damp.cpp`），没有它 locomotion 一定抖；半天。

### 2.2 Root Motion（仅 Generic，不做 Humanoid）

- **配置**：`AnimationComponentRes` 加 `apply_root_motion`、`root_motion_bone`（名字；空 = 骨骼根）；每状态/子项加烘焙开关 `bake_xy / bake_z / bake_yaw`（Unity `clipmuscle.h:129-184` 的 LoopPose / KeepOriginal 简化版）。
- **载入期预计算**：每片段根骨骼的起始/结束变换 StartX/StopX（Unity `clipmuscle.cpp:1480`），用于跨循环的整圈位移。
- **提取（每片段每帧）**：只采样根骨骼那一个通道的 prev / curr 两个时刻（很便宜）；处理循环：`Δ = (Stop − Prev) + k·(Stop − Start) + (Curr − Start)`，倒放、Once 夹紧各一分支（Unity `ComputeClipTime` `clipmuscle.cpp:694`、`EvaluateRootMotion` `:1106`）。旋转只取绕上轴的 yaw。多片段按**与姿势相同的权重**加权求和。
- **剥离**：被提取的分量要从根骨骼姿势里减掉（固定在起点），否则"位移一份 + 模型自己又走一份"。
- **坐标系（风险点）**：通道是"相对 bind pose 的增量"（`FbxAnimation.h:182-188`），在根骨骼**父空间**里。要乘根骨骼父链的 bind 变换转到物体空间，再乘物体朝向转到世界。引擎是 **Z-up**（`MotorComponent.cpp:383` 用 `UNIT_Z` 做竖直），FBX 常见 Y-up + Armature 绕 X 转 90°。这一步写错表现为"往天上走 / 横着走"。
- **消费**：
  - 有 Motor：`apply_root_motion` 时用根位移**替换** `m_DesiredDisplacement` 的水平分量，竖直仍由 Motor 的重力/跳跃管，然后照常走 `CalculateTargetPosition`（复用 CharacterController 碰撞）。Yaw 交给 Motor，并让 `Character::Tick` 的旋转写入在该模式下让路（G6）。
  - 无 Motor（NPC / 过场）：直接写 Transform（Unity 无 CharacterController 时的内建行为）。
  - Timeline 覆盖期间：root motion 关闭（`AnimationComponent::Tick` 的 Timeline 分支早退，语义最清楚）。
- **顺序**：Motor 在自己的 Tick 里**主动拉** `AnimationComponent::ConsumeRootMotion()`；若 Animation 还没 Tick 就先让它 Tick（带本帧 dt、带"本帧已 Tick"标记防重入）。不去改 `GameObject::Tick` 的全局顺序。

---

## 3. 任务拆解与人日

### M0 前置（两块共用）— 2.5 – 4.5 人日

| ID | 任务 | 人日 | 验收 |
|---|---|---|---|
| P0.1 | ~~`AnimationClip` 加 `duration`（秒）字段；导入器写入；`AnimatorState.clip_length <= 0` 时回落到片段时长~~ | ~~0.5 – 1~~ | **✅ 已落地 2026-10-28**，见下 |
| P0.2 | `BlendStateWithClipData.blend_clip` 改存 `const AnimationClip*`；`Skeleton::ApplyAnimation` 的 3 个 vector 改成员 scratch（入口先 `clear()`）；`AnimatorInstance` 初始化时把路径解析成指针，帧内不再查 `std::map<eastl::string>` | 1 – 1.5 | `AnimationSkinningSmokeTest` + `AnimatorSmokeTest` 全绿；姿势逐位不变 |
| P0.3 | 测试资产：一套 CC0 角色（Quaternius / Kenney 等），至少 idle / walk / run（原地）+ walk_forward（带根位移）+ 一个转身；导入 → skeleton / skel map 检查 | 1 – 2 | 编辑器里能单独播放每个片段；**注意授权，别用不能进仓库的素材** |

### 落地记录 · P0.1 片段时长落盘（2026-10-28）

- `AnimationClip::duration`（秒）**追加在 Transfer 末尾**。必须追加而非插入：序列化器的位置式读取按写入顺序走，插在 `node_channels` 之前会让每一份既有资产错位。旧片段无此节点 ⇒ SafeBinaryRead 返回 kNotFound ⇒ 保持 0。**0 的语义是"未知"，不是"零长"**，消费方必须回落到作者填的长度。
- 导入器：`BakeStack` 取 `baked->playback_duration`（帧数被 `cap` 截断时秒数依然正确 —— 截断降的是采样率，不是时长）。**写 asset 的 lambda 是逐字段重建 `clip_data` 的，不显式拷贝就会被静默丢掉** —— `duration` 一开始就是这么丢的。
- 消费方分两层：`AnimatorInstance` 新增 `m_StateLength`（6 处 `clip_length` 读取全部改走它）+ `SetStateClipLength`，**本身仍然不碰资产**（一个控制器被多个实例共享，且要能 headless 驱动）；`AnimationComponent::PostLoadResource` 才去加载片段补齐。**作者填的长度优先** —— 那是有意的改时长覆盖，用资产悄悄顶掉会让 Inspector 字段变成谎话。
- 验证（测试 P8，10 项）：时长过真实序列化器往返；`total_frame` 与通道未受扰动；未填长度的状态确实从片段取到 0.8s；**200 帧 @1/60 = 3.3333s 跑在 0.8s 循环上，相位落在 0.1667（期望 0.1667）**；并且拿仓库里真实的旧资产 `W2_CrouchWalk_Aim_F_Loop_IP`（23 帧 58 通道，写于本字段之前）证明向后兼容 —— 这是手搓夹具给不了的证据。
- 回归：pipeline 39 / skinning 46 / animator 50 全绿，**P7 哈希仍 `17764afb96eb6b4b`**（Debug 与 Release 皆是），ZEditor 编译通过。

### M1-A BlendTree 运行时 — 6 – 8.5 人日

| ID | 任务 | 人日 |
|---|---|---|
| B1 | 数据模型：`AnimatorState` 扩展 + `BlendTreeChild` + 序列化兼容 | 0.5 – 1 |
| B2 | 1D + Direct 权重 | 0.5 |
| B3 | 2D Cartesian + SimpleDirectional 权重（移植 Unity） | 1.5 – 2.5 |
| B4 | `AnimatorInstance` 接入：有效时长同步、事件按状态归一化时间触发、交叉淡化 N+M、零权重剔除、阻尼 SetFloat | 1.5 – 2 |
| B5 | `BlendTreeSmokeTest`（目标 ≥ 50 个 Check：阈值点 one-hot、区间线性、越界夹紧、2D 采样点 one-hot 且 Σw=1、时长同步、树↔片段交叉淡化权重和）+ CMake（`add_subdirectory` + 两条 `REMOVE_ITEM`）+ CI 两步 | 1 – 1.5 |
| B6 | PIE 验证：Motor 的 `getSpeedRatio()` 喂 `speed` 参数，idle/walk/run 1D 树 | 1 |

### M1-B Root Motion 运行时 — 6 – 9.5 人日

| ID | 任务 | 人日 |
|---|---|---|
| R1 | 配置字段（组件开关、根骨骼名、烘焙开关）+ 根骨骼解析 | 0.5 – 1 |
| R2 | 提取核心：prev/curr 根通道采样、Start/Stop 预计算、跨循环、倒放、Once 夹紧、多片段/交叉淡化加权、yaw 提取 | 2 – 3 |
| R3 | 根骨骼姿势剥离 + **坐标系转换**（父空间 → 物体 → 世界，Z-up） | 1 – 2（风险项） |
| R4 | 消费：Motor 接入（替换水平位移、yaw 让路 `Character::Tick`）、无 Motor 直写 Transform、Timeline 期间关闭、拉取式顺序 | 1.5 – 2.5 |
| R5 | `RootMotionSmokeTest`：合成片段（根 1 m/s、长 1 s 循环），dt=0.3 跑 10 帧累计必须正好 3.0 m（跨 2 次循环）；倒放 −3.0；Once 停在终点；两片段 1 m/s 与 3 m/s 各 0.5 权重 ⇒ 2 m/s；剥离后根骨骼水平位置恒定；yaw 90°/s 累计 | 1 – 1.5 |

> M1-B 的 PIE 验证（带根位移的 walk 真往前走、不滑步）合在 R4 里。

### M2 编辑器 + 脚本 — 4 – 6.5 人日

| ID | 任务 | 人日 |
|---|---|---|
| E1 | Animator 窗口：状态 Motion 类型下拉、参数选择、子项表（复用 Clip/SkelMap 拾取器，`PickField` 带子项下标）、阈值/坐标输入、"按片段自动算阈值"；图节点画 BlendTree 标记 | 2 – 3 |
| E2 | 2D 混合空间预览控件（子项点位 + 可拖动当前参数点 + 实时权重） | 1.5 – 2 |
| E3 | 脚本 API：阻尼 `SetFloat`、`SetApplyRootMotion`、`GetDeltaPosition/GetDeltaYaw`；（可选）TS 侧 `OnAnimatorMove` 回调 | 0.5 – 1.5 |

---

## 4. 风险

| 风险 | 概率 | 后果 | 对策 |
|---|---|---|---|
| 根骨骼坐标系（Y-up/Z-up、Armature 旋转、单位缩放 cm↔m） | 高 | R3 +1~2 天 | R5 先用合成片段把数学钉死，再上真资产；真资产只看方向和距离对不对 |
| 不先做 P0.2 就上 BlendTree | 中 | 多角色时帧时间翻倍、难定位 | P0.2 放第一步，后面所有改动都在新路径上 |
| CharacterController 只有"重叠就拒绝" | 确定 | Root motion 撞墙卡死 | 本评估**不包含**；接 Jolt `PhysicsCharacter` 另起专项（估 3 – 5 天） |
| `Character::Tick` 与 Motor 都写旋转 | 中 | Yaw 型 root motion 被覆盖 / 抖动 | R4 显式让路，PIE 里专门测原地转身 |
| 素材授权 | 中 | 无法入库 | 只用 CC0；Mixamo 素材只放本地、不进 VCS |
| 另一会话同时在改 E: | 中 | 提交冲突 | 按 AGENTS 约定点名 `git add` |

## 5. 明确不包含

Humanoid / Avatar / 重定向（Unity 的 Humanoid root motion 是基于质心和双脚的，另一套）、动画层与 Avatar Mask 层权重、同步层、Additive、IK、嵌套 BlendTree、Mirror、FreeformDirectional 2D、Jolt `PhysicsCharacter` 接线、网络同步。

## 6. 建议顺序

`P0.2 → P0.1 → B1 → B2 → B5(1D部分) → B4 → B3 → B5(2D) → P0.3 → B6 → R1 → R2 → R5 → R3 → R4 → E1 → E3 → E2`

理由：先把采样热路径修干净（之后所有数字都可信）；BlendTree 1D 先打通再上 2D；root motion 先用合成数据把数学钉死（R5 在 R3 之前），坐标系问题留给真资产时只剩一个变量。
