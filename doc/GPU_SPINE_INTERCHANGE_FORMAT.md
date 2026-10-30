# GPU-Spine 中间格式规格（ZEngine ⇄ Unity 共用）

> **状态：草案 v0.1 —— 先定规格，再写代码。**
> 本文只定义**数据格式与 shader 数学**，不涉及任何一端的实现。
> 目的：ZEngine（C++ 导入器）与 Unity（C# 导入器）产出**同一个容器**，
> 两端 shader 跑**同一条公式**，从而视觉一致、可互相对拍。
>
> **版本锁定：Spine 3.8**（已决策，见 §1.1）。
> 所有引用 spine-cpp 行为的地方均已核对
> `G:/OSGame/unity-engine-source/Modules/Spine/spine-cpp`，行号见文末「事实来源」。

---

## 0. 为什么要有这一层

CPU 蒙皮下**每个实例必须持有自己的一份顶点缓冲**，因为各实例动画状态不同、顶点结果不同 ——
这是 N 份动态 VB 的根本原因。把顶点变换搬到 GPU 后，VB 里存的是**绑定姿势**（与实例无关），
于是可以按 SkeletonData **共享一份**，N 份塌缩成 1 份。

> **GPU 化不是"省算力"，而是"使共享成为可能"。** 只做 GPU 蒙皮但每个实例仍保留自己的 Mesh，
> 内存几乎不变甚至略增（静态 VB 多了骨骼槽）。收益的来源是 **共享** —— 实例化是另一件事，见 §0.1。

因此本规格的核心不是"怎么把矩阵传到 GPU"，而是**如何把"与实例无关"的部分和"与实例有关"的部分切干净**。

### 0.1 共享与实例化是两件事（不要捆在一起读）

| | 省什么 | 地位 |
|---|---|---|
| **共享**（§3 静态 VB / §5 每实例数据） | **内存 + 每帧上传带宽**：静态几何按 asset 只有一份，实例只剩骨骼表（41 骨 × 24 B ≈ 1 KB）+ 可见绘制项列表（20 B/项）+ deform 增量（均值 122 B） | **收益主体** |
| **实例化**（§4 合批键 → 一次 instanced draw 覆盖多个绘制项） | draw call | 可选优化 |

> **实例化不是共享的前提，共享的成立条件是"GPU 蒙皮"，不是"实例化"。**
> 不实例化 = 每个可见绘制项一次 `DrawIndexed`，draw call 涨回去；
> 但顶点缓冲仍然只有一份、每帧仍然只传骨骼表 ⇒ **内存与带宽的收益一点不少**。
> 上面那句"共享 + 实例化"应读作"共享（主体）**叠加**实例化（可选）"。

### 0.2 硬前提：宿主渲染器必须允许 per-instance 骨骼参数

⚠️ **整套收益只在"一次 draw 里不同实例可以取不同骨骼表"时成立** —— 即 §5 的
`bone_base` / `root_index` 能被逐实例寻址。宿主一旦要求 Spine **交付顶点去参与合并**
（UGUI Canvas 的 `OnPopulateMesh` / `CanvasRenderer::SetMesh` 模型），
顶点就必须在 CPU 上蒙完皮，于是：

| 收益项 | 走顶点合并后 |
|---|---|
| 共享（N 份 → 1 份） | **归零** —— 合并后的大 VB 本来就是一份，N 份塌缩的收益原本不存在 |
| 上传少 2.62x / geo 快 2.34x（§17） | **归零** —— 每帧照写 posed 顶点，蒙皮回 CPU |
| 资产内部去重（§3.2 linked mesh / 皮肤变体） | **存活** —— 与渲染路径无关，顶点池变小照样省 |
| anim 段慢 1.43x（§17，anim 0.70x） | **仍然要付** |

⇒ 收益全没、成本还在 = **净亏**。所以**接入前先确认渲染路径**，不要做完格式才发现宿主吃不下。
唯一例外：宿主若肯用自定义 shader + 全局骨骼 buffer，则"几何不脏就不重建"能让
**不带 deform 的资产**吃到"几何常驻 + 每帧只传 1 KB 骨骼表"；但 deform 覆盖 **93% 资产**
（§14.1）—— deform 一动几何就变 ⇒ 每帧重建 ⇒ 这条路只对剩下一小部分成立。

**OSGame 现状满足这条前提**（2026-10-26 核对 `G:/OSGame/unity-engine-source/Modules/Spine`）：
`UnitySkeletonGraphic` 是 `Behaviour`（`UnitySkeletonGraphic.h:21`），几何走
`m_MeshRenderer->SetSharedMesh(m_Mesh)`（`UnitySkeletonGraphic.cpp:377-390`，`m_Mesh` 是
`MarkDynamic()` 的**每实例** Mesh）；`m_CanvasRenderer` 只在 `BakeMesh()` 里用于
`EnableRectClipping`（`:432-451`），**从不 `SetMesh`** ⇒ 它挂在 UI 层级里（`RectTransform`）
但**不参与 Canvas 合批**，本来就是"一个骨架一份世界空间动态 VB"
（代价清单见 `doc/GPU_SPINE_V7_ANIMATION.md` §5.3）。
⇒ **"走实例化会打断合批"在 OSGame 这条链上不成立 —— 没有合批可被打断。**
若将来改成 Canvas 顶点合并路径，本规格的收益必须按上表重新评估。

> 注：实例化真正的代价不是"打断合批"（宿主本来就没合批时无从打断），而是
> **Spine 与其它 UI 元素交错遮挡时不能再按层级逐层排序**。这要按实际 UI 结构判断，
> 不能一概而论 —— 它影响的是"要不要实例化"，不影响"要不要共享"。

### 范围
- 静态几何（绑定姿势 + 骨骼槽 + 权重）的布局，以及**独立 UV 通道**（§3.2）
- 每帧每实例数据（骨骼表 + 槽位染色）的布局
- 两端必须一致的蒙皮公式
- 绘制项 / 混合模式 / 裁剪的处置

### 非范围（本规格刻意不解决）
- 动画解算（`AnimationState::update` + `Skeleton::updateWorldTransform` **永远在 CPU**。
  它是带约束的顺序遍历，IK / 物理 / path 是迭代求解器，并行要分层 barrier，收益微秒级）
- VAT 烘焙（**明确否决**：frames × verts × 3 张纹理，1000 顶点 × 100 帧 ≈ 1.2 MB/动画/资产。
  本项目的痛点是内存，VAT 会让内存暴涨，且换皮 / 动画混合 / IK / 挂点 / 裁剪全废）
- ClippingAttachment 的 GPU 化（多边形裁剪 + 三角化会**产生新三角形**，VS 做不了）

---

## 1. 术语

| 词 | 含义 |
|---|---|
| **asset** | 一份 `SkeletonData` + `Atlas` 的构建产物，按资产共享，**唯一一份静态 VB** |
| **instance** | 一个活着的骨架（有自己的 `AnimationState` / 骨骼表 / 槽位染色） |
| **draw item** | 一次可绘制的最小单位 = 一个附件的可见部分（4 顶点四边形，或一段网格） |
| **bone table** | 每帧每实例上传的骨骼世界变换数组，6 float/骨 |
| **rigid** | `_bones.size() == 0` 的附件：整块由**槽位骨骼**单一矩阵变换 |
| **weighted** | `_bones.size() != 0` 的附件：每顶点多个影响，每个影响**有自己的绑定坐标** |

---

## 1.1 版本锁定：Spine **3.8**（已决策 2026-10-21）

**决策**：GPU 化在 **3.8** 内做，升 4.x **单独排期**。理由不是"3.8 更好"，而是**可归因** ——
同一版本内做，回归能归因到"GPU 化"本身；版本升级 + 算法替换同时上，出问题无法二分。

**⚠️ 2026-10-22 修订：导入器同时接受 3.7.x**（`ClassifySpineVersion()`，4.x 仍明确拒绝）。
理由不是改了决策，而是**3.7 与 3.8 的 `.skel` 字节布局逐段比对完全一致**
（骨骼 / 槽位 / IK / transform / path / 皮肤 / 附件 / 事件 / 动画），
唯一的差别是槽位暗色判据：

| | 判据 | 出处 |
|---|---|---|
| 3.7 | `int darkColor = ReadInt(input); if (darkColor != -1) hasSecondColor = true;` | 3.7 `spine-csharp/src/SkeletonBinary.cs:190-192` |
| 3.8 | 读 4 字节，`!(r==ff && g==ff && b==ff && a==ff)` 才算有暗色 | `spine-cpp/src/spine/SkeletonBinary.cpp:186-189` |

⇒ **同一个解析器 + 一个版本分支**，不是两套。容器的 `spine_runtime` **写实** 30700 / 30800
（把 3.7 记成 30800 会让读端按 3.8 的判据解释暗色，而那个差别**不报错**）。
本工程 OSGame 的 782 个资产**全是 3.7.83**，此前 0 个能导入。

**判据（本机已核对，`G:/OSGame/unity-engine-source/Modules/Spine/spine-cpp`）**

| 判据 | 出处 |
|---|---|
| `Vector<unsigned short> _bones`，源码注释原话：*"unsigned short is ample; a skeleton cannot reach 65535 bones."* | `VertexAttachment.h:79-81` |
| 带 `applyDeform(VertexAttachment*)` 与 `getId()` / `const int _id`（3.8 的 deform 重做：附件带 id，deform 可跨 skin 复用） | `VertexAttachment.h:66,69,86` |
| `computeWorldVertices` 同时提供 2 参与 6 参（start/count/offset/stride）两种重载 | `VertexAttachment.h:53-63` |

**这条锁定实际钉死了什么**

| 项 | 3.8 下的取值 | 出处 |
|---|---|---|
| 骨骼索引元素类型 | **`uint16`**（不是 uint32） | `VertexAttachment.h:81` |
| 单骨架骨骼数上限 | 65535（`uint16` 索引的天然上限，与源码注释同义） | 同上注释 |
| `_vertices` 每影响三元组步长 | 3 float `(vx, vy, weight)` | `VertexAttachment.cpp:97-112` |
| deform 长度 | weighted **每影响 2 float**、rigid 每顶点 2 float（`deformLength = verts.size()/3*2`） | `SkeletonJson.cpp:1071`、`SkeletonBinary.cpp:976` |
| 混合模式枚举 | `Normal=0 / Additive / Multiply / Screen`，共 4 种 | `BlendMode.h:35-38` |
| 裁剪 | **只有 CPU 实现** | §1.1 下文、§7.2 |

**顺带解决一项待拍板**：3.8 里裁剪**只有 CPU 实现** —— `SkeletonClipping::clipTriangles()` 会
**重写顶点 / 三角形 / UV**（`SkeletonClipping.h:47-49` 的 `clipTriangles`，`:52-54` 的 `getClippedVertices/Triangles/UVs`），
即裁剪会**产生新的顶点和新的三角形**，VS 无法表达。
所以 §13 的"Clip 走 CPU fallback"不是折中方案，而是 **3.8 下的唯一选项** —— 它从"待拍板"降级为**格式约束**：

> **带裁剪的槽位不进 GPU 路径**，走 CPU 生成的动态 VB（§7）。同一帧内两种路径可混跑。

**迁移风险（写在这里，因为以后一定会有人问）**
- 4.x 的 `_bones` 是 `Vector<int>` ⇒ 索引类型 uint16→uint32，顶点格式、骨骼表寻址、导入器全要动。
  ⚠️ **这一条未在本机核实**（本机无 4.x 源码），升级时必须重新核对，不要照抄。
- OSGame 项目里的 spine-unity 运行时是 **3.7**
  （`Client_proj/Project/Assets/OSGame/3rd-Party/Spine/version.txt` 已核对：
  *"works with data exported from Spine Editor version: 3.7.xx"*）。
  若将来要让 ZEngine 吃 OSGame 的导出资源，`.skel` 二进制版本与 3.8 不同，导入器需要版本分支。

---

## 2. 顶层：一个 asset = 一个容器

```
SpineGpuAsset
├─ Header              魔数 / 版本 / spine runtime 版本 / 各段偏移
├─ BoneTable           骨骼名（仅调试与挂点，不参与渲染）
├─ VertexBuffer        静态：绑定姿势 + 骨骼槽 + 权重       ★ 按资产共享、逐字节去重
├─ UvBuffer            静态：float2 × uv_count              ★ 独立通道，见 §3.2
├─ IndexBuffer         静态：uint16
├─ DrawItemTable       静态：按 draw order 排好的绘制项
├─ SlotTable           槽位 → 附件列表（换装 / 显隐用）
├─ AtlasPages          图集页引用（纹理句柄或路径）
└─ ClipData            裁剪多边形数据（保留给 CPU fallback）
```

运行时每帧每实例只有两块：

```
SpineGpuInstance
├─ BoneTable     6 float × boneCount        （每帧上传；多实例打包进同一个大 buffer）
└─ InstanceList  CPU 每帧紧凑化的可见绘制项列表（16 B/项）
```

---

## 3. 静态顶点缓冲

### 3.1 两种顶点格式（**不要统一成一种**）

spine-cpp 有两个完全不同的分支，GPU 端必须分别对应：

**rigid（`VertexAttachment.cpp:71-87`）** —— `_bones` 为空，
`_vertices` 是 `(x, y)` 对，整块由 `slot._bone` 一个矩阵变换：

```
worldVertices[w]     = vx * a + vy * b + x;
worldVertices[w + 1] = vx * c + vy * d + y;
```

**weighted（`VertexAttachment.cpp:97-112`）** —— 每顶点 `n` 个影响，`_vertices` 里
**每个影响一份三元组 `(vx, vy, weight)`**（步长 3），权重**不在 `_bones` 里**：

```
wx += (vx * bone._a + vy * bone._b + bone._worldX) * weight;
wy += (vx * bone._c + vy * bone._d + bone._worldY) * weight;
```

> ⚠️ **最容易写错的一点**：weighted 的绑定坐标是**每影响一份**，不是每顶点一份。
> 影响 0 和影响 1 的 `(vx, vy)` 分别在各自骨骼的局部空间里，**不相等**。
> 只给每顶点存一份 bindPos 会得到能编译、能跑、但形状全错的网格。

| 格式 | 布局（按偏移顺序） | 步长 |
|---|---|---|
| `kVtxRigid` | `bindPos f32×2` (0-7) | **8 B** |
| `kVtxWeighted`（v7） | `influence_offset u32` (0-3) + `influence_count u32` (4-7) | **8 B** |

**v7 起 weighted 顶点只是一条形如 `(影响流下标, 影响个数)` 的引用** ——
真正的 `(bindPos, boneIdx, weight)` 在**两条独立的影响流**里（chunk 21 / 22），
两条流**共用同一个条目下标**（就是 `influence_offset`）：

| chunk | 内容 | 每条 | 用途 |
|---|---|---|---|
| 21 `influence_bind` | `bindPos f32×2` | 8 B | `bind.Load2((influence_offset + k) * 8)` |
| 22 `influence_bone` | `boneIdx u16` (0-1) + `weight unorm16` (2-3，**v17**；v16 及以前是 unorm8 + u8 pad) | 4 B | `bone.Load((influence_offset + k) * 4)`，整条一次 Load 再拆 |

> ⚠️ **UV 不在这里。** 顶点只存"几何 / 引用"，UV 在独立的 chunk 4 里
> （`float2` × `uv_count`，8 B/条），由 `DrawItem::uv_offset` 索引。见 §3.2。

> **为什么两条流要分开、而不是交错成一条 12 B**：
> `ByteAddressBuffer::Load2` 要求偏移 **8 字节对齐**。交错的 12 B 步长会让
> 第 1、3、5…条的 bindPos 落在 12、36…（不是 8 的倍数）⇒ 只能退化成标量 Load。
> 同理 `bone` 那条必须**整条 4 B 一次 Load 出来再拆**，不能在 `@2` 处单独 Load ——
> 那个地址不是 4 字节对齐的。

> **为什么从"定长 4 槽 48 B"改成"逐顶点变长"**（`doc/GPU_SPINE_V7_ANIMATION.md` §1.5）：
> OSGame 782 个资产实测 —— 每顶点影响数最大 **15**、均值 **2.20**，
> >4 的顶点占 4.1%（24 458 / 594 372）⇒ 定长 4 槽直接卡死 **297 个资产**；
> 定长 16 槽能装下但要 176 B/顶点，与"降内存"的目标相反。
> 变长后均值 ≈ 8 + 12 × 2.20 ≈ **34 B/顶点**（48 B 的 0.72×），
> 且 shader 循环 **2.2 次**而不是固定 4 次 ⇒ **内存和 ALU 一起省**。

- **每段 VB 只有一种格式**，绘制项记录 `vertex_format`。合批只能在同一格式内进行。
- `MAX_INFLUENCES = 255` —— v7 起它**不再是"固定槽位数"**，只是"单个顶点影响数的硬上限"，
  用来挡"数据坏了"（把 n 读成 300 之类）。正常资产永远碰不到它。
  导入器遇到 `n > 255` 仍然**报错并报出附件名**，不允许静默截断。
- **权重归一化**：导入期把该顶点 `influence_count` 个 unorm16 调整到**和恰好 65535**
  （v17；v16 及以前是 unorm8 / 255），余量补给权重最大的那个，运行时统一 `/ 65535.0`。
  不要在 shader 里做除法归一化，也不要写成 `* (1.0 / 65535.0)`（倒数先舍入 ⇒ 差 1 ulp）。
  **体积没变**：原来 @3 的 pad 被吃掉。
  为什么升：端到端蒙皮对拍里 weighted 顶点最大误差 **5.21 骨架单位**（与 deform 无关），
  全来自 u8 量化（步长 1/255 × 骨骼到顶点的距离 ~1000 单位）；u16 降到 **0.071**（§15）。
  （每个影响一次除法，且引入与 CPU 不一致的舍入）。
- **顶点色不在静态 VB 里** —— Spine 的颜色是**槽位色**（`slot.getColor()`），每实例可变，属于实例数据。

### 3.2 UV 是独立通道（**不在顶点里**）

UV 存在单独的 `uv_buffer`（chunk 4）：`float2` × `uv_count`，8 B/条。
绘制项带 `uv_offset`（单位是**UV 条目**，不是字节）；第 k 个顶点的 UV 在
`uv_buffer[uv_offset + vertexIndex]`，`vertexIndex` 由索引缓冲给出。

> ⚠️ **v13 起 `uv_offset` = `uv_unit - vertex_base`，按 uint32 回绕**。索引缓冲里存的是
> **段内绝对**下标（= 附件本地下标 + 该附件顶点在段里的起点 `vertex_base`），而 UV 是按
> **附件本地**下标排的（从 `uv_unit` 起）。所以只有把 `vertex_base` 预先减掉，上面这条公式
> 才成立：`uv_offset + index` 在 C++ / C# / GLSL 的 uint 加法下正好回到 `uv_unit + 本地下标`。
> v12 及以前落的是 `uv_unit` —— 只在 `vertex_base == 0` 时对。OSGame 实测 119 737 个绘制项里
> **112 562 个** `vertex_base != 0`，其中 **39 548 个**会读出 UV 表，其余"只是采错一块贴图"，
> 都不报错（直到第一次真的画到屏幕上才暴露）。读端现在逐项校验 `uv_offset + index < uv_count`。

**为什么要拆开**：让**几何相同的附件共用同一段顶点**。两类最常见的重复：

- **linked mesh**（附属网格）：继承父附件的顶点 / 骨骼 / 三角形 / regionUV，
  但用**自己的**图集 region 重算 UV。
- **换装变体**：不同皮肤往往只换贴图 region，网格一模一样。

UV 编在顶点里时，UV 不同 ⇒ 顶点不能共用 ⇒ 只能整份复制（旧 §14.2）。
拆开之后顶点池按**逐字节完全相同**去重，UV 各取各的一份。

- **顶点数 ≠ UV 条目数**（顶点被共用时 UV 更多）。不要拿 `vertex_count` 去算 UV 数组长度，
  用 chunk 4 的大小 / 8。
- 代价：shader 里多一次间接读取（先取 `indexBuffer[...]` 得到顶点下标，再取 UV）。

**去重只认"完全相同"，绝不做近似匹配**：近似意味着"我觉得这两个差不多"，
差一点点形状就静默错，且没有任何报错。宁可少共享，不可错共享。
（实现用 FNV-1a 哈希**缩小比较范围**，判定始终是逐字节比较 —— 哈希撞了也不会出错。）

### 3.3 索引
`uint16`。Spine 附件极小，单个骨架超过 65535 顶点的情况用 `flags` 里的
`kIndex32` 打开 uint32 变体 —— 默认 uint16 是因为 `std::vector<uint32>` 索引在
Unity 原生模块里是实打实的浪费（~8 B/顶点）。

### 3.4 骨骼 setup（chunk 10，36 B/骨，v8）

```
SpineGpuBone                 36 B
  x              f32    @0
  y              f32    @4
  rotation       f32    @8    度（spine 全线用度，不是弧度）
  scale_x        f32    @12   ⚠️ 默认 **1** 不是 0
  scale_y        f32    @16
  shear_x        f32    @20
  shear_y        f32    @24
  length         f32    @28   IK / 编辑器挂点；不参与蒙皮
  parent         i16    @32   -1 = 根；否则必须 **< 自己的下标**
  transform_mode u8     @34   SpineTransformMode（抄 TransformMode.h:34-40）
  _pad           u8     @35   落盘必须写 0
```

字段集合抄 `BoneData.h:110-115`；`transform_mode` 的 5 个取值抄 `TransformMode.h:34-40`
（normal / onlyTranslation / noRotationOrReflection / noScale / noScaleOrReflection），
语义在 `Bone.cpp:126-185` 的 `updateWorldTransform()` switch —— 决定父骨骼的世界变换
里哪些分量传给子骨骼。json 侧的字段名是 **"transform"**（不是 transformMode），
默认值与解析顺序抄 `SkeletonJson.cpp:165-189`；二进制侧 8 个 float 的顺序是
**rotation, x, y, scaleX, scaleY, shearX, shearY, length**（`SkeletonBinary.cpp:157-164`）
—— 与 json 的书写顺序**不同**，写反了不报错，只是每根骨骼摆位全错。

**为什么必须有它**：动画是**相对 setup pose 的增量**。没有 setup 的 x/y/rotation/
scale/shear，时间轴给的"rotation += 30"无从合成；IK 的 bone length、约束的 offset
也都挂在 setup 上。容器里缺了它，动画阶段就只能回头读源文件 ⇒ 容器不自包含。

- **顺序不变量 `parent < index`**。有它之后正向 for 一遍就是拓扑序，
  `updateWorldTransform` 不需要排序 / 递归 / 栈。反序列化必须校验：
  违反说明骨骼表被重排过或读坏了，而重排的后果是"骨骼下标在容器里有两种含义"。
- 读端还要校验：`chunk 10 条数 == header.bone_count`。⚠️ **"chunk 不存在"本身不报错**
  （空 chunk 不落盘），少了这个等式，读端会得到 0 根骨骼，而后面所有 `bone_idx`
  全部越界 —— 越界在 shader 里不报错，只是蒙皮取到 0 号骨骼。
- `transform_mode` 越界（> 4）必须报错：参考实现是 `static_cast` 后在 switch 里
  **没有 default 分支**（`Bone.cpp:126-185`）⇒ 那根骨骼的世界变换静默保持单位阵。
- json 里认不出的 `transform` 串也必须报错：参考实现 5 个 `if` 都没有 `else`
  （`SkeletonJson.cpp:173-189`）⇒ 静默留在 Normal，整棵子树的继承行为悄悄变掉。

**实测（781 个 OSGame 容器，`doc/scripts/measure_zspg_chunks.py`）**：
骨骼总数 **32 036**（均 41.0 根/资产，最深树 **33** 层），chunk 10 合计 **1.10 MB**。
transform 模式分布：normal **93.68%** / noRotationOrReflection **6.26%** /
onlyTranslation 0.04% / noScale 0.02% / **noScaleOrReflection 0 次**。
⇒ 5 个值里 4 个在真实资产上出现过，`noScaleOrReflection` 没有 —— 它仍要按
`Bone.cpp:172-185` 实现，但**没有真实资产能给它的解算结果背书**
（S4 阶段要用合成夹具对拍）。

### 3.5 动画（chunk 11-16，v9）

五块之间全是**下标**引用。完整设计见 `doc/GPU_SPINE_V7_ANIMATION.md` §3.2-§3.4
与 §7.0.3；这里只记容器这一侧的布局。

```
SpineGpuAnim（chunk 11）        20 B/条
  name_offset     u32   @0    进 chunk 1 的**字节**偏移
  timeline_first  u32   @4    进 chunk 12 的**条**下标
  timeline_count  u32   @8
  duration        f32   @12   = max(各时间轴最后一帧的 time)
  _pad            u32   @16   落盘必须写 0

SpineGpuTimeline（chunk 12）    32 B/条，全局一张表，动画按区间引用
  type            u16   @0    SpineTimelineType（TimelineType.h:35-49 原值）
  target          u16   @2    骨骼 / 槽位 / **附件记录** / 约束 下标（随 type 变）
  frame_count     u32   @4
  frame_first     u32   @8    ⚠️ 进 chunk 13 的 **float** 下标，**不是**帧号
  curve_first     u32   @12   进 chunk 14 的**段**下标
  bezier_first    u32   @16   进 chunk 15 的**条**下标
  bezier_count    u32   @20
  payload_offset  u32   @24   进 chunk 16 的**字节**偏移
  payload_count   u32   @28   条目数（变长三类是帧数，其余恒 0）

chunk 13  帧        f32[]     帧数据平面数组；第 n 帧的时间 = frames[frame_first + n*ENTRIES]
chunk 14  曲线类型  u8[]      每段一个：0 linear / 1 stepped / 2 bezier（CurveTimeline.cpp:42-44）
chunk 15  bezier    SpineGpuBezier[]，16 B/条（cx1, cy1, cx2, cy2）
chunk 16  变长载荷  字节       attachment 名 / deform 稀疏帧 / drawOrder 偏移
```

- ⚠️ **`frame_first` 是 float 下标**：各类每帧的 float 数不同（rotate 2 / translate 3 /
  color 5 / twoColor 8 …）⇒ **不存在全局统一的"帧"单位**。用"帧"去算下标，第一条
  （偏移 0）看不出错，第二条起就读到别人的帧数据，且不报错。
- ⚠️ **动画数量不进 header**：64 B header 的 16 个 u32 已经排满，而 chunk 11 是定长
  20 B ⇒ 数量 = chunk 11 大小 / 20，读端照此反推并校验整除。
- **时间轴顺序是规范化的**：按 (type, target) 升序、稳定排序。两条输入路径的顺序本来
  就不一样（二进制槽位→骨骼，json 同序但条目顺序取决于文件里键的书写顺序），
  不规范化就无法做"skel↔json 逐字节相同"的对拍。
- `target` 为 deform 时是**附件记录**（chunk 7 行号）下标 —— 一条记录 =
  (皮肤, 槽位, 附件) 三元组，正好是 deform 时间轴的寻址键。
- 变长三类（attachment / deform / drawOrder）只有 **time** 进 chunk 13，其余在 chunk 16，
  这样"取第 n 帧的时间"对所有类型都是同一个公式。
- attachment 帧里的"没有附件"是 **0xFFFFFFFF 不是 0** —— 0 是 chunk 1 里第一个
  字符串的合法偏移，填 0 会让空帧变成"挂上某个真实附件"且不报错。
- 读端必须查**每一个**区间（帧 / 曲线 / bezier / 载荷 / 动画的时间轴区间）：
  下标越界在 C++ 与 C# 里都不崩，只会播到别人的数据。

**实测（781 个 OSGame 容器）**：chunk 11-16 合计 **166.72 MB**，占容器的 **80.8%**
（其中 deform 载荷 122.81 MB 独占 59.5%）。时间轴 **254 471** 条、动画 **1 932** 个；
15 类时间轴里真实出现的 **11 类**；曲线 linear 58.26% / bezier 36.57% / stepped 5.17%。
完整表格见 `doc/GPU_SPINE_V7_ANIMATION.md` §7.0.3。

### 3.6 槽位 setup（chunk 17，40 B/槽，v10）

```
SpineGpuSlot（chunk 17）        40 B/条，条数 == header.slot_count
  color_r/g/b/a   f32×4 @0     默认 (1,1,1,1)（SlotData.cpp:39）
  dark_r/g/b/a    f32×4 @16    默认 (0,0,0,0)（SlotData.cpp:40）
  setup_attachment_name_offset u32 @32   进 chunk 1 **第 4 组**的字节偏移
  flags           u32   @36    bit0 = has_dark（其余位必须 0）
```

- **为什么必须落盘**：color / twoColor 时间轴写的是**绝对**颜色
  （`ColorTimeline.cpp:110` `slot._color.set(r,g,b,a)`），没被时间轴命中的槽位停在
  **setup 色**。容器里原本只有一个 `uses_tint_black` 布尔、**没有颜色值**
  ⇒ 解算只能默认纯白，而**纯白与 setup 白在多数资产上完全看不出区别**。
  实测 **139 / 781 个资产（17.8%）至少有一个非纯白 setup 色**。
- ⚠️ **dark 的 alpha 恒为 1**（只要这个槽位有暗色）—— 参考实现写死：
  二进制 `SkeletonBinary.cpp:188` `set(r/255, g/255, b/255, 1)`；
  json `SkeletonJson.cpp:230` `darkColor.a = 1`。
  二进制读进来的第 4 个字节**只**用于判"有没有暗色"，从不作为 alpha 落地。
  json 的 `"dark"` 是 **6 位** hex（只有 rgb），`"color"` 是 **8 位**（rgba）。
- ⚠️ **`has_dark` 必须由显式 flag 记录，不能由 dark 是否全 0 推断**：
  默认 dark 就是 (0,0,0,0)，而 3.7 的判据是 `读到的 int32 != -1`
  （`SkeletonBinary.cs:190-192`）⇒ **0x00000000（纯黑）照样是"有暗色"**。
- ⚠️ `setup_attachment_name_offset` 是 **attachment 时间轴的复位基线**
  （`AttachmentTimeline.cpp:65-68`）：`time < frames[0]` 时槽位复位到 setup 附件，
  **不是**"隐藏"。空名 = `kNoAttachmentName = 0xFFFFFFFF`（**不是 0**）。
- **字符串表多一组**：第 4 组「槽位 setup 附件名」，`slot_count` 条，
  **空名也占一条**（否则按 `slot_count` 切分会整体错位）。
  顺序：骨骼名 → 皮肤名 → 槽位名 → **槽位 setup 附件名** → 附件名 → 图集页名 →
  动画名 → 动画附件名。
- 读端校验：大小是 40 B 的整数倍；条数 == `header.slot_count`；
  `setup_attachment_name_offset` 落在字符串块内（或 == `kNoAttachmentName`）；
  flags 未知位必须 0。

**实测（781 个 OSGame 容器）**：槽位 **56 704** 个（均 72.6/资产），chunk 17 合计
**2.16 MB**（容器的 **1.03%**）；带暗色 **96** 槽位 / 5 个资产；
有 setup 附件名 **33 055** 槽位（**58.29%**）。

---

### 3.7 约束 setup（chunk 18 / 19 / 20，v11）

**为什么必须落盘**：约束**每帧都跑**，与时间轴无关。实测 **481 / 781 个资产
（61.6%）至少有一条约束**（transform 2082 条、IK 344 条、path 333 条）——
容器里没有它们，动画解算只能"摆骨骼"，而"约束没跑"的表现是**姿势不动**，不报错。

#### 3.7.1 约束 setup（chunk 18）

```
块头 16 B：ik_count, transform_count, path_count, bone_pool_count（各 u32）
紧随其后：SpineGpuIkConstraint[ik_count]       20 B/条
          SpineGpuTransformConstraint[tf_count] 56 B/条
          SpineGpuPathConstraint[path_count]    40 B/条
          uint16 bone_pool[bone_pool_count]      2 B/条（三类**共用**一个池）

SpineGpuIkConstraint（20 B）
  order      u16 @0   三类约束**共用**一个排名空间（不是数组下标）
  bone_count u16 @2
  bone_first u32 @4   进 bone_pool 的**下标**（不是字节）
  target     u32 @8   目标**骨骼**下标
  mix        f32 @12  默认 1（SkeletonJson.cpp:290）
  bend_dir   i16 @16  1 或 -1
  flags      u16 @18  bit0=compress bit1=stretch bit2=uniform

SpineGpuTransformConstraint（56 B）
  order/target/bone_first/bone_count 同上；target 也是**骨骼**下标
  offset_rotation/x/y/scale_x/scale_y/shear_y  f32 @12..@32
  rotate_mix/translate_mix/scale_mix/shear_mix f32 @36..@48
  flags u16 @52  bit0=local bit1=relative
  pad   u16 @54  落盘必须 0

SpineGpuPathConstraint（40 B）
  order/bone_count/bone_first 同上
  target_slot   u32 @8   ⚠️ 是**槽位**下标（SkeletonBinary.cpp:249 取 `_slots[...]`）
  position_mode u8 @12   0=fixed 1=percent
  spacing_mode  u8 @13   0=length 1=fixed 2=percent
  rotate_mode   u8 @14   0=tangent 1=chain 2=chainScale
  pad           u8 @15   落盘必须 0
  offset_rotation/position/spacing/rotate_mix/translate_mix  f32 @16..@32
  path_index    u32 @36  见下
```

- ⚠️ **path 的 target 是槽位、IK / transform 的 target 是骨骼** ——
  字段名两边都叫 "target"（甚至偏移都是 @8），但**取值域不同**。
  拿 `bone_count` 去查 path 的 target、或拿 `slot_count` 去查 IK 的 target，
  都会漏掉另一半越界。`SpineGpuLayout.Validate()` 里钉了一条
  「两者偏移相同但 index space 不同」的检查提醒这一点。
- ⚠️ **`order` 必须是 0..N-1 的一个排列**（N = 三类约束总数）。
  参考实现的 `updateCache()` 按 `i = 0..N-1` 逐个找 `order == i` 的那条，
  **找不到就静默跳过**（`Skeleton.cpp:147-175`）；负数还会以 `int == size_t`
  比较变成天文数字，同样静默。⇒ **导入期与读端都显式报错**。
- ⚠️ **`bend_dir` 在二进制里是 sbyte、在 json 里是 `bendPositive` 布尔**。
  json 侧 `"bendPositive": false` ⇒ **-1**（`SkeletonJson.cpp:291`
  `getInt(..., 1) ? 1 : -1`）。Spine 的 json 解析器把 true/false 写进同一个
  `_valueInt`（`Json.cpp:165,173`）⇒ 读端若只认数字，布尔会回落到默认值 **1**，
  **不报错、方向反了**。实测踩到过一次，靠 skel↔json 逐字段对拍抓到。
- json 的 mode 串认不出时（三个 if 都没 else）会停在构造函数的默认值；
  缺键时回落 percent / length / tangent —— **两者不一致** ⇒ 我们选择显式报错。

#### 3.7.2 执行顺序（chunk 19）

```
块头 8 B：step_count, reset_count（各 u32）
SpineGpuUpdateStep[step_count]  4 B/条：kind u8 @0 | pad u8 @1 | index u16 @2
uint16 reset[reset_count]       2 B/条
kind: 0=bone 1=ik 2=transform 3=path
```

`Skeleton::updateCache()`（3.7 `Skeleton.cpp:134-178`）算出来的 `_updateCache`
**原样落盘**，运行时照着遍历就行。

- **为什么不落"让运行时重排"**：重排规则有 `sortBone` 的递归 + 三类约束共用的
  `order` 两层；运行时再实现一遍就是给自己多一次写反的机会，而写反的表现是
  "姿势略偏"，查不出来。落**结果**是唯一能对齐的。
- ⚠️ `_updateCacheReset` 也要落：它是"遍历之前要把哪些骨骼复位到 setup"的列表
  （`Skeleton.cpp:200-211`）。少复位会让上一帧的应用值残留，表现是**动作拖影**。
- ⚠️ **只在有约束时落盘**。无约束时 `updateCache()` 的结果**可证恒等于**
  `0..bone_count-1`（因为 `parent < index` ⇒ 正向遍历就是拓扑序），reset 为空。
  读端按同一规则重建 ⇒ Bake 内存与 Read 内存一致、读→写逐字节相同。
  这不是兜底，是等价 —— 少写 781 个资产里 300 个的纯冗余数据。
- **反向约束**：有约束（chunk 18 非空）却**没有** chunk 19 ⇒ 容器坏了，**必须报错**
  （静默退化成"没有约束"，与"约束没排进去"在字节上完全看不出区别）。

#### 3.7.3 PathAttachment 几何（chunk 20，40 B/条）

```
块头 16 B：att_count, bone_count, vertex_float_count, length_count（各 u32）
SpineGpuPathAttachment[att_count]  40 B/条
uint16 bone_pool[bone_count]
float  vertices[vertex_float_count]
float  lengths[length_count]

SpineGpuPathAttachment（40 B）
  slot_index  u16 @0
  skin_index  u16 @2   ⚠️ 皮肤**必须**落盘（见下）
  flags       u16 @4   bit0=closed bit1=constantSpeed bit2=weighted
  pad         u16 @6   落盘必须 0
  vertex_count u32 @8
  bone_offset/count         u32 @12/@16
  vertex_offset/float_count u32 @20/@24
  length_offset/count       u32 @28/@32   count == vertex_count / 3
  name_offset  u32 @36  ⚠️ 进字符串表第 8 组
```

- **它不产生绘制项**（与 boundingbox / point / clipping 一样），但**必须烘焙** ——
  没有它就算不出 `PathConstraint::computeWorldPositions()`。
  这是 v11 把 path 从"只留个名字"升级成"烘焙但不画"的原因。
- ⚠️ **皮肤必须落盘**：`PathConstraint::update()` 用的是 `_target->getAttachment()`
  （`PathConstraint.cpp:76-80`）—— 槽位**当前**挂的附件，而"当前"取决于激活的皮肤。
  只按槽位找会在换装后拿到别的皮肤的曲线。
- ⚠️ **附件名必须落盘**：同一个槽位在同一皮肤下挂**两条** path 附件是**合法的**
  （实测 6 / 781，如 `K_Lubandashi_Spine` 的槽位 `EFFTAIYILIZI` 挂了
  `EFFTAIYILIZI` 与 `EFFTAIYILIZI2`）。`update()` 取的是"当前"那条，
  而"当前"会被 attachment 时间轴改掉 ⇒ 静态烘焙只能给出 **setup 状态下**的答案
  （`path_index`），运行时换装 / 换附件后必须**按名字重新查**。
- ⚠️ `path_index` 的判据是"**名字 == 槽位的 setup 附件名**"，不是"这个槽位的
  第一条 path"。取第一条在单条时看不出区别，在两条时会去摆一条别人的曲线
  （不报错，只是动作飘）。
  setup 附件不是任何 path ⇒ `kNoPathAttachment`，约束在 setup 状态下不生效
  （`update()` 直接 return）。**`kNoPathAttachment = 0xFFFFFFFF`，不是 0** ——
  0 是 chunk 20 里第一条的合法下标。
- **字符串表多一组**：第 8 组「path 附件名」，`att_count` 条，
  与 chunk 20 的记录**一一对应**（不去重）。
  ⚠️ 它必须排在「动画附件名」**前面** —— 后者是"剩下的全归它"，没有计数，
  排在它后面的组读端切不出来。
  顺序：骨骼名 → 皮肤名 → 槽位名 → 槽位 setup 附件名 → 附件名 → 图集页名 →
  动画名 → **path 附件名** → 动画附件名。

#### 3.7.4 读端校验（chunk 18 / 19 / 20）

- 三个块头都带 count ⇒ "chunk 不存在"与"chunk 被截断"能分开；
  **净载荷大小必须与块头算出来的相等**（差一个字节都会让后面的池整体错位）。
- flags / pad：未知位与 pad 必须 0（脏字节会让跨语言逐字节对拍对不上）。
- 骨骼区间 `[bone_first, bone_first + bone_count)` 必须落在池里；
  **池里的每个索引必须 < `bone_count`**；IK / transform 的 `target < bone_count`；
  path 的 `target_slot < slot_count`。
- `order` 必须是 0..N-1 的排列；`bend_dir` 只能是 ±1；mode 枚举不能越界；
  step 的 `kind ≤ 3`、约束下标不越界。
- `path_index` 必须是 `kNoPathAttachment` 或 < `att_count`
  （放在 chunk 20 之后交叉检查，因为它要引用那边的条数）。
- `length_count == vertex_count / 3`（`SkeletonBinary.cpp:550`）。

**实测（781 个 OSGame 容器）**：带约束 **481 / 781（61.6%）**；
IK **344** 条 / transform **2082** 条 / path **333** 条；约束骨骼池 **3150** 条；
chunk 18 **0.14 MB**、chunk 20 **0.08 MB**、三块合计 **0.33 MB**（容器的 **0.16%**）。
执行顺序 **27 468** 步（带约束的资产共 24 941 根骨骼 ⇒ 约束给遍历表加了
**2 527** 步，**9.2%**）。PathAttachment **690** 条（11592 个顶点 float、
1921 个 length float）。`path_index` 全部有效（333 / 333）⇒ 这些资产的
path 约束在 setup 状态下就生效。全部 781 个容器：pad 非 0 / bend_dir 非法 /
kind 越界 / order 非排列 **均为 0**。

---

## 4. 绘制项表（静态）

按 (皮肤, `skeleton.getDrawOrder()` 的**槽位顺序**) 排好 —— 先 default 皮肤的全部附件，
再下一个皮肤；皮肤内按槽位顺序。每项：

```
DrawItem                     32 B（v14 起；v6-v13 是 28 B，v5 是 24 B）
  index_offset  u32   @0
  index_count   u32   @4
  uv_offset     u32   @8   （单位：**UV 条目**，不是字节；v13 起已减去 vertex_base，见 §3.2）
  vertex_format u8    @12  (kVtxRigid / kVtxWeighted4)
  blend_mode    u8    @13  (见 §8；v13 起 = 槽位的真实混合模式，v12 及以前恒为 normal)
  atlas_page    u16   @14  (v13 起 = 附件 region 所在的图集页，v12 及以前恒为 0)
  slot_index    u16   @16
  attachment_id u16   @18
  skin_id       u16   @20
  kind          u8    @22  (kStatic / kFallback)
  _pad          u8    @23
  clip_index    u16   @24  chunk 9 的行号；**没被裁时是 kNoClipIndex = 0xFFFF**（见 §7.2）
  vertex_base   u16   @26  (v14) 顶点在所属段里的起点：索引 = 附件本地下标 + vertex_base
  deform_parent u16   @28  (v14) linked mesh 且 inheritDeform 时 = 父附件 id，否则 kNoDeformParent = 0xFFFF
  vertex_count  u16   @30  (v14) 附件顶点数；deform 长度 rigid = 2×它，weighted = 2×这些顶点的影响数之和
```

v14 的三个字段都是 GPU deform（§7.1）的前提，且**都无法从别的字段反推**：
`vertex_base` 在 v13 只被折进了 `uv_offset`；"最大索引 + 1"在尾部顶点没被三角形引用时会少算顶点数；
inheritDeform 在烘焙期只决定 `kind`，运行时认不出"挂在父附件上的 deform 时间轴该不该作用于自己"。
读端校验：索引落在 `[vertex_base, vertex_base + vertex_count)`、该区间落在所属段内、
`deform_parent` 为"没有"或同一槽位里另一个附件；附件记录满足 `attachment_id == 行号`。

**没有 `skin_id` 就没法换装**：同一个 (槽位, 附件名) 在不同皮肤下是两个绘制项，
光靠 `slot_index + attachment_id` 分不出该画哪一个。
**没有 `uv_offset` 就没法共用顶点**：几何相同的附件共用同一段顶点，UV 各取各的（§3.2）。

合批键 = `(atlas_page, blend_mode, vertex_format, kind)`。
连续同键的绘制项可合成一次实例化绘制。

> **DrawOrderTimeline 例外**：运行时改绘制顺序的动画无法用静态表表达。
> 导入器检测到该时间轴就在 `flags` 置 `kHasDrawOrderTimeline`，
> 该资产**整个走 CPU 路径**（不要试图在 shader 里重排，代价远大于收益，且 UI Spine 极少用）。

---

## 5. 每帧每实例数据

### 5.1 骨骼表（核心）

每骨 6 个 `f32`，顺序固定：

```
struct Bone { float a, b, c, d; float worldX, worldY; };   // 24 B 紧凑
```

- 来自 `Bone::getA()/getB()/getC()/getD()/getWorldX()/getWorldY()`（`Bone.h:189-209`），
  即 `Skeleton::updateWorldTransform()` 之后的世界变换。
- **索引 = `skeleton.getBones()` 的下标**，必须与导入器一致。
- `bone_stride` 记录在 Header 里：
  - `24` —— 紧凑（structured buffer / SSBO / instance buffer）**默认**
  - `32` —— std140 UBO 兼容（vec4 + vec4，第二组后两分量留空）

> **默认用 structured buffer / instance data（SSBO），不用纹理。** 两条理由要分开看：
>
> 1. **"纹理会额外吃显存"—— 成立（打包浪费，不是翻倍）。** RGBA32F 是 16 B/texel，
>    与 SSBO 同量级，多出来的主要是**打包浪费**：骨骼 24 B/骨 落在 1.5 个 texel 上，
>    按 2 texel 算就是 32 B/骨。本项目的痛点正是内存 ⇒ SSBO 优先。
> 2. **"纹理采样会引入与 CPU 不一致的舍入"—— ⚠️ 只对"带滤波的采样"成立，对 `texelFetch` 不成立。**
>    （2026-10-26 修正，原表述把两者混为一谈。）`texelFetch` 用**整数 texel 坐标、不滤波**，
>    RGBA32F / R32UI 是**原值返回** ⇒ 与 SSBO 逐位一致，不引入任何舍入。
>    前提：格式必须是 **fp32 / u32**（RGBA32F / R32UI / R32F），**不能退到 fp16**；
>    ES3.0 core 里 RGBA32F 可采样，NEAREST 与 `texelFetch` 合法（线性滤波才需要
>    `OES_texture_float_linear`）。
>
> ⇒ **准确的表述是：否决"纹理 + 归一化坐标 + 滤波"这条路径，不是否决纹理本身。**
>
> **SSBO 换成纹理要付的代价（跨资产合批）**：一次 draw 里 sampler 绑定的纹理是固定的
> ⇒ 不同资产的实例不能再合进同一次 draw，§5.1 末尾那句"十万个角色少数几个 draw call"
> 的前提没了。能救的是 **array texture**（ES3.0 core，`texelFetch(t, ivec3(x, y, layer), 0)`
> 的 **layer 允许动态索引** —— 它是坐标的一部分，不是 sampler 索引），代价是
> ①同 array 各层尺寸必须一致 ⇒ 按资产大小分桶 + padding 浪费；
> ②`MAX_ARRAY_TEXTURE_LAYERS` 保证只有 **256**，781 个资产必须分桶 + 槽位分配；
> ③动态数据（b7 deform / b8 cpu_verts）每帧要走 `TexSubImage` 而不是 `BufferSubData`。
>
> **没有 SSBO 时的备选（GLES3 / 移动端）**：静态几何（b0-b3）纹理化 + 骨骼表走 UBO
> （41 骨 × 24 B ≈ 1 KB，远低于 ES3.0 保证的 16 KB `MAX_UNIFORM_BLOCK_SIZE`），
> 每实例一次 draw。**UBO 装不下整份静态数据**（中位 33.7 KB / p90 97.7 KB，§17），
> 所以静态部分只能走纹理，不能走 UBO；**顶点属性定长展开则不可行**
> （每顶点影响数最大 15，§1.5 —— 定长 4 槽卡死 297 个资产）。
> 这条路的取舍见 §5.4。

- 所有实例的骨骼表**打包进同一个每帧大 buffer**，实例只带 `bone_base`（以骨为单位的下标）。
  这样不同骨架实例也能合进同一次绘制 —— 这是"十万个角色少数几个 draw call"的前提。

### 5.2 实例项（每可见绘制项一项，**20 B**）

```
DrawInstance
  draw_item_index u32     // 索引静态绘制项表
  bone_base       u32     // 该实例骨骼表在大 buffer 里的起始骨下标
  tint            u32     // RGBA8，槽位色（直 alpha，非预乘）
  dark_tint       u32     // RGBA8，tintBlack（slot.hasDarkColor() 才有意义）
  root_index      u32     // 索引每帧 root 表（单位是「根变换条目」，不是字节）
```

- 5 个 u32 = 20 B，**无尾部填充**（对齐刚好是 4）。
- `root_index` **追加在末尾**而不是紧跟 `bone_base` —— 保持既有四个字段的偏移不变，
  序列化代码不用整体挪位。
- 同一实例的多个绘制项（一个骨架有多个）共享同一个 `root_index` 与 `bone_base`。

**CPU 每帧紧凑化（compact）**：只把**当前可见**的绘制项追加进列表。
换装、槽位隐藏、裁剪走 fallback 的项都由 CPU 决定去留 ——
**shader 里不需要任何"是否可见"分支**。

### 5.3 根变换

实例的世界变换（UI 里的位置/缩放/旋转）作为额外的 2×3 仿射 `root`，
在蒙皮**之后**施加：`p = root.apply(skinned)`。
（不能预乘进骨骼表 —— 骨骼表存的是骨骼世界变换，乘进去会二次缩放。）

**存放位置（已拍板，§13.1 第 6 项）**：每实例一个 root，放进**每帧 root 表**，
由实例项的 `root_index` 索引。

```
RootTransform { float a, b, c, d; float tx, ty; }   // 24 B 紧凑
  x' = a*x + b*y + tx
  y' = c*x + d*y + ty
```

- 步长 24 B；若走 std140 UBO 变体则跟 `bone_stride` 一致改成 32。
- 与骨骼**完全同构**（都是 2×3 仿射、都是 24 B），但**语义不同**：
  一个是骨骼世界变换、一个是实例根变换。`SpineGpuFormat.h` 里保持成两个类型，
  防止互相换用 —— 换用了能编译、能跑，只是角色整体位移/缩放悄悄错。

### 5.4 无 SSBO 的备选（GLES3 / 移动端）—— 待验证，未拍板

> **状态：2026-10-26 记为"备选方案"，不是已决策路线。** 只有 §5.1 默认的 SSBO 路径
> 在目标平台上不可用时才走这里；**上 §5.1 的默认不变**。

触发条件：目标后端没有 SSBO（GLES3 / ES 3.0 没有 SSBO；Unity 的 `StructuredBuffer` /
`ByteAddressBuffer` 要 SM5.0+，GLES3 直接编译失败，不是降级）。

**必须随机访问的东西只有一处**：weighted 附件的**变长影响流**（每顶点影响数最大 15，§1.5）
⇒ 必须有一个"按计算下标随机读"的大缓冲。于是：

| 通道 | 无 SSBO 时怎么放 | 说明 |
|---|---|---|
| b4 骨骼表 | **UBO** | 41 骨 × 24 B ≈ 1 KB，远低于 ES3.0 保证的 16 KB `MAX_UNIFORM_BLOCK_SIZE`。std140 下 `float[]` 的 stride 是 16 B ⇒ 按 `vec4` 数组存再手动取分量（或用 `bone_stride = 32` 变体），不要用 `float bones[]` |
| b0-b3 静态几何（顶点 / 影响流 / UV） | **纹理 + `texelFetch`** | **UBO 装不下**（中位 33.7 KB / p90 97.7 KB，§17）；**顶点属性定长展开不可行**（影响数最大 15）。格式必须 **RGBA32F / R32UI**（fp32 / u32 原值，见 §5.1 的修正：不能退到 fp16） |
| b5 绘制项 / b6 实例 | UBO（每帧）或纹理 | 每帧量小（20 B/项、96 B/实例），UBO 足够 |
| b7 deform / b8 CPU 裁剪顶点 | 纹理 | 每帧更新走 `TexSubImage`（不是 `BufferSubData`） |

**付掉的代价**：一次 draw 的 sampler 绑定是固定的 ⇒ **跨资产合批没了**。

- 对 **OSGame 现状不亏**：§0.2 已确认它本来就是"一骨架一 MeshRenderer"，没有合批可丢；
- 若仍要跨资产合批，只能上 **array texture**（ES3.0 core，layer 可动态索引），
  代价是同 array 各层尺寸一致（分桶 + padding）、`MAX_ARRAY_TEXTURE_LAYERS` 保证只有 256
  （781 个资产要分桶 + 槽位分配器）。

**保留的收益**：静态几何仍按资产**共享**（内存收益完整）；每帧上传仍是"骨骼表 + 可见项 + deform"，
不退化成全量 posed 顶点（§17 的 2.62x / 2.34x 保住）。

⚠️ **落地前必须先做最小验证**（不要先改容器格式）。完整的门与对拍项见
**`doc/GPU_SPINE_NO_SSBO_VERIFY.md`**（G0 平台探针 / G1 最小 `.shader` / G2 对拍项 / G3 代价量化 /
Go-No-Go 判据）。最要紧的三件事：①RGBA32F / R32UI 在该机型确实可 `texelFetch`；
②`texelFetch` 与 SSBO **逐位**一致；③array texture 的 layer 数上限实测值（ES3.0 只保证 256）。

⚠️ **在 OSGame 上，本节的 `texelFetch` 属于"现网未使用"的特性 ⇒ 必须先走 §5.6 的例外申请（路线 B）。**
批准前不要落地；驳回时的兜底是 §5.5 的 `tex2Dlod`。

### 5.5 OSGame 可用特性集（2026-10-26 实测，硬约束）

> **规则：GPU-Spine 在 OSGame 上只能使用 OSGame 现网 shader 已用到的特性；未用到的特性一律不可用。**
> 下表是**实测结果，不是按平台文档推断**。
>
> 口径：`I:\OSGame\Client_proj\Project\Assets`（发布分支），递归 `*.shader`（**1784**）+
> `*.cginc` / `*.hlsl` / `*.glslinc`（675）= **2459 个文件**。

| 特性 | 现网用量 | 归属 | 判定 |
|---|---|---|---|
| `cbuffer` / `CBUFFER_START` | 209 / 70 | 通用 | ✅ UBO 可用 |
| `tex2Dlod` | 274 处 / 199 个文件 | 遍布 | ✅ 纹理取数可用（**兜底通道**） |
| GPU instancing：`multi_compile_instancing` 31 / `UNITY_VERTEX_INPUT_INSTANCE_ID` 67 / `UNITY_ACCESS_INSTANCED_PROP` 10 | 31 个 `.shader`，**含 UI 的 `Sprites-DefaultCustom.shader`** | UI / 场景 / PBR / Uber 特效 | ✅ 实例化可用 |
| `#pragma target 3.0` | **1120** | 主流档 | ✅ |
| `#pragma target 3.5` | 256 | 同上 | ✅（`SV_VertexID` / `SV_InstanceID` 需要的档位） |
| **`texelFetch`** | **0** | —— | ❌ **需申请例外**（§5.6） |
| `sampler2DArray` / `TEXTURE2D_ARRAY` / `sampler3D` | 0 | —— | ❌ |
| `SV_VertexID` | 2 | 仅 `Yarp/.../VertexShaderMainOS.cginc`、`ShaderPassMetaOS.cginc` | ❌ 非通用路径 |
| `#pragma kernel` / `numthreads`（compute） | 0 | —— | ❌（§0 已定动画解算在 CPU，本就不需要） |
| `precise` | 0 | —— | ❌ **且无法申请**（GLSL ES 3.00 没有 `precise`） |
| `StructuredBuffer` / `ByteAddressBuffer`（SSBO） | 31 处 / 9 文件 | `DecalHelper.cginc`(10)、UGC `GIBaker/*`(17)、`ScreenSpaceRayTrace.cginc`(1)、`PBRScene_SGameTerrain.cginc`(1)、Editor 工具(1) | ❌ 全在**离线烘焙 / 高端 / Editor** 路径，非通用运行时 |

**决定性：目标后端配置**（`ProjectSettings\ProjectSettings.asset:709-719` + 枚举取自
`unity-engine-source\Runtime\Export\Graphics\GraphicsEnums.cs:1436-1466`）：

| 平台 | `m_APIs` 原始值 | 解码 | Unity 版本 |
|---|---|---|---|
| **AndroidPlayer** | `0b000000` | **OpenGLES3 = 11**（0x0b） | 2022.3.5f1 |
| iOSSupport | `10000000` | Metal = 16（0x10，`m_Automatic: 1`） | 同上 |
| WindowsStandaloneSupport | `02000000` | Direct3D11 = 2 | 同上 |

⇒ 三条推论：
① 移动端**没有 Vulkan** ⇒ `StructuredBuffer` / `ByteAddressBuffer` 在 Android 上确实不可用，
§5.4「去 SSBO」的前提成立；② `texelFetch` 是 **ES3.0 core** 指令，而 Android 正是 ES3 ⇒
申请有硬依据（不是"靠运气"）；③ `precise` 在 GLES3 与 Metal 上都没有 ⇒
**移动端拿不到 bit-exact**（§5.6）。

**另注（最保守的一档）**：OSGame 现网 **Spine 自身的 17 个 shader 全部未写 `#pragma target`**
（仅 `Spine-SkeletonLit.shader` 显式写 2.0）⇒ Unity 默认 **2.5**，是全套 shader 里最保守的一档。
⇒ 即使只申请 target 3.5，对 Spine 而言也是"提档" —— 需单独确认低端机不受影响。

### 5.6 例外申请（路线 B，2026-10-26 选定）

在 §5.5 的规则下，GPU-Spine 要在 OSGame 落地**必须申请以下例外**（未用到的特性不可直接用）：

| 申请项 | 理由 | 状态 |
|---|---|---|
| `#pragma target 3.5` | 现网 **256 个** shader 已在用（3.0 更是 1120）⇒ 严格说不是"新特性"，只是 Spine 自己没声明。要 3.5 是因为 `SV_VertexID` / `SV_InstanceID` 需要它（Unity 口径，待 G0 实测确认） | 待批准 |
| `texelFetch` | **ES3.0 core 指令**，target 3.5 起即可用；**不是扩展** —— 线性滤波才需要 `OES_texture_float_linear`，`texelFetch` 不需要。拿到它 §5.4 才成立 | 待批准 |
| ~~`precise`~~ | **GLSL ES 3.00 没有 `precise`** ⇒ 无法申请 | 放弃 ⇒ 见下 |

⚠️ **`precise` 放弃的后果**：FMA 收缩无法禁止 ⇒ **逐位一致无法保证** ⇒
`SpineGpuEmulateVertex`（`SpineGpuBatch.h:140`）/ `SpineGpuRefParityTest` 的 bit-exact 判据
在 GLES3 后端上不成立，需降级为**误差阈值**（底噪取法见 §17："无 deform"那格）。
（2026-10-26 追加判定）Android = **OpenGLES3**、iOS = **Metal**（§5.5 实测），**移动端没有 Vulkan 后端**
⇒ `precise` 在移动端**两个后端都拿不到** ⇒ bit-exact **只在 PC（D3D11）上维持**，移动端一律走误差阈值。

**驳回时的降级顺序**：

1. 例外全过 ⇒ §5.4 原样落地（静态几何 `texelFetch`，逐位性最好）；
2. `texelFetch` 被否 ⇒ 静态几何改走 `tex2Dlod` + Point 采样 + 尺寸取 2 的幂（§5.5 合规），
   逐位性更脆弱，**必须单独对拍**（`GPU_SPINE_NO_SSBO_VERIFY.md` G2 第 11 项）；
3. 连兜底轨也不可用，或误差超出底噪 ⇒ **回退路线 A**：GPU-Spine 只做 ZEngine / PC / 编辑器，
   OSGame 维持 CPU 蒙皮现状。

---

## 6. Shader 数学（唯一权威公式）

以下伪代码是**两端必须逐字实现**的公式。ZEngine 与 Unity 的视觉一致性完全依赖它。

```hlsl
// bones: 全局骨骼 buffer（所有实例打包）
// inst : 当前实例项
float2 SkinVertex(uint vertexIndex, uint drawItemIndex)   // UV 不参与，见 §3.2
{
    float2 p = 0;

    if (vertex_format == kVtxRigid)
    {
        // 整块由槽位骨骼单一矩阵变换（对应 VertexAttachment.cpp:80-85）
        Bone B = bones[inst.bone_base + drawItem.slot_bone_index];
        p.x = bindPos.x * B.a + bindPos.y * B.b + B.worldX;
        p.y = bindPos.x * B.c + bindPos.y * B.d + B.worldY;
    }
    else  // kVtxWeighted4
    {
        // 逐影响累加，顺序 = 影响存储顺序（对应 VertexAttachment.cpp:102-110）
        [unroll]
        for (int i = 0; i < 4; ++i)
        {
            float w = weight[i] / 65535.0;      // v17：unorm16（v16 及以前 / 255.0）
            Bone B = bones[inst.bone_base + boneIdx[i]];
            p.x += (bindPos[i].x * B.a + bindPos[i].y * B.b + B.worldX) * w;
            p.y += (bindPos[i].x * B.c + bindPos[i].y * B.d + B.worldY) * w;
        }
    }

    return RootApply(p);   // §5.3
}
```

三条硬性约束：

1. **混合的是"变换后的位置"，不是矩阵。**
   先 `Σ wᵢ · (Bᵢ · vᵢ)`，绝不能先 `Σ wᵢ · Bᵢ` 再变换 —— 两者数学上不等价，
   后者是新手最常见的错，而且不报错、只是形状微妙地不对。
2. **累加顺序必须与 spine-cpp 一致**（按影响下标升序遍历）。
   f32 加法不满足结合律，换序会引入 ulp 级差异，破坏与 CPU 路径的对拍。
3. **FMA**：编译器可能把 `vx*a + vy*b` 收缩成 `fma`，与 CPU 的"先乘后加"舍入不同。
   要求与 CPU 路径**逐位一致**时，必须关掉收缩（HLSL：`precise` 或拆成独立 mul/add；
   GLSL/Cg 侧同理）。只要求视觉一致时可放宽，容差见 §12。

---

## 7. 三类附件的处置

| 附件 | 静态 VB | 说明 |
|---|---|---|
| **Region** | 烘焙 4 顶点 | `RegionAttachment::computeWorldVertices` 用 `_vertexOffset[8]` 的 br/bl/ul/ur 四点，由**槽位骨骼**单一矩阵变换 ⇒ 天然是 rigid。UV 由图集页（可能带 90° 旋转）算好，写进独立 UV 通道（§3.2）。 |
| **Mesh（刚性）** | 烘焙，rigid | `_bones` 为空，整块由槽位骨骼变换。**8 B/顶点**。 |
| **Mesh（蒙皮）** | 烘焙，weighted | v7 起顶点 **8 B（引用）** + 影响流 12 B/影响 ⇒ 均值 ≈ **34 B/顶点**（实测均值 2.20 个影响）。 |
| **Clipping** | `_ClipData` 保留 | `SkeletonClipping::clipTriangles()` 重写顶点/三角形/UV（`SkeletonClipping.h:47-54`）⇒ **产生新三角形，VS 做不了** ⇒ **CPU fallback**：该项走 `kFallback`，顶点由 CPU 算好上传每帧动态 VB。这是 3.8 的唯一选项，不是折中（§1.1）。**回退的粒度是槽位**（不是整个资产），作用域见 §7.2。 |
| **FFD / deform** | 同左 | 顶点每帧都变 ⇒ 静态 VB 里那份**绑定姿势**不算数 ⇒ 该项走 `kFallback`。见 §7.1。 |

Region 的 4 个顶点**烘焙进静态 VB**（而不是用共享单位四边形 + 每实例 offset/uv），
因为静态部分按资产共享、成本一次性，而每实例数据才是内存大头。

### 7.1 deform（FFD）

deform 时间轴命中的附件，顶点**每帧都不一样**。容器里这些绘制项标 `kind = kFallback`
（"需要每帧数据"），并在 `flags` 置 `kHasDeform`。**只标被命中的项，不是整个资产。**

> **S6（v14）起运行时不再为它们走 CPU 动态 VB**（原方案"整份世界坐标 CPU 动态 VB"已否决，
> 见 `GPU_SPINE_V7_ANIMATION.md`）：
> * 解算器（`SpineGpuSamplePose` 第 8 步）每帧给出**生效的** deform 数组（附件本地顺序，
>   与参考 `Slot::getAttachmentVertices()` 逐元素同义），按"当前附件 == 时间轴附件，或
>   inheritDeform 且父附件 == 时间轴附件"判定作用于谁；
> * 渲染器把它们拼成一块上传（b7），绘制项带一个 uint 回绕偏置 `deform_base`：
>   rigid `deform[deform_base + 2·vi]`（`deform_base = 段起点 − 2·vertex_base`），
>   weighted `deform[deform_base + 2·(off + k)]`（`deform_base = 段起点 − 2·infl_base`）；
> * shader 里 rigid **整体替换** bindPos、weighted **逐影响加**偏移，蒙皮公式不变 ——
>   **静态 VB 仍然共享**，每帧多传的只是"这一帧真在动的那几项的 deform 数组"。
>
> 前提（都由测试钉住）：一个附件的影响流在 chunk 21/22 里**整块连续、保持原顺序**
> （影响流去重按整块做），所以 `off − infl_base` = 前面顶点的影响数之和 = 参考实现的 `f/2`。
> 绘制项因此要带 `vertex_base` / `vertex_count` / `deform_parent`（v14，§4）。

#### 两种附件的 deform 语义**不一样**（照错就是静默错形状）

| | 存的是什么 | 运行时怎么用 | 长度 |
|---|---|---|---|
| **rigid** | **绝对**局部坐标（setup + 偏移） | 整体**替换** `_vertices` | 每顶点 2 float |
| **weighted** | **偏移** | 逐影响**相加**：`vx = vertices[b] + deform[f]` | 每**影响** 2 float |

出处：`VertexAttachment.cpp:72-73`（rigid 替换）、`:122-123`（weighted 相加）、
`SkeletonJson.cpp:1071` / `SkeletonBinary.cpp:976`（长度公式）。

⚠️ weighted 是**每影响 2 个**，不是每顶点 2 个。长度公式里的 `verts.size()/3`
除的是**已剥掉 n 的**每影响三元组 ⇒ `size/3` = 总影响数。运行时也对得上：
`f = skip << 1` 里的 `skip` 累加的是 `n`（影响数），循环里 `f += 2` 是**逐影响**
（`VertexAttachment.cpp:115-127`）。

#### inheritDeform 必须沿 linked mesh 传播

判据照抄 `MeshAttachment::applyDeform`（`MeshAttachment.cpp:94-95`）：

```
this == source || (_inheritDeform && _parentMesh == source)
```

⇒ 父附件被 FFD 动到时，**开了 inheritDeform 的继承方顶点同样每帧变**，得一起回退。
linked mesh 的父**也可以**是 linked mesh，所以传播要循环到不动点。
json 里这个开关的字段名是 **`"deform"`**（不是 `"inheritDeform"`），默认 1
（`SkeletonJson.cpp:572`）；二进制是紧跟 parent 的一个 bool（`SkeletonBinary.cpp:594`）。

#### 导入期必须挡住的四件事

参考实现在这四处**都不安全**，导入器一律**报错**而不是照抄：

| 情况 | 参考实现的行为 |
|---|---|
| deform 引用的皮肤不存在 | `findSkin` 返回 NULL 后紧接着 `skin->getAttachment(...)` ⇒ **空指针**（`SkeletonJson.cpp:1051`）；二进制是 ` _skins[i]` 不检查下标（`:959`） |
| deform 引用的附件不存在 | 这个**倒是**报错："Attachment not found"（`:1061-1065` / `:966-970`） |
| deform 指向 **region** | **无检查的** `static_cast<VertexAttachment*>`（`:1067` / `:972`）；而 3.8 的 `RegionAttachment` 继承自 `Attachment`、**不是** `VertexAttachment`（`RegionAttachment.h:46`）⇒ **未定义行为**。运行时有 RTTI 检查，会**静默不生效**（`DeformTimeline.cpp:72-74`） |
| 帧的 `offset + 顶点数` 超过 deform 长度 | `deformed[v] = ...` 无边界检查 ⇒ **越界写**（`:1092-1099`） |

### 7.2 裁剪作用域（per-slot）

裁剪也是"画不进静态 VB"（`clipTriangles()` 会重写顶点 / **三角形** / UV，
三角形个数都变了，`SkeletonClipping.h:47-54`），但它的作用范围是**一批槽位**，
不是某一个附件 ⇒ 导入期必须先把作用域算出来，再把作用域内的绘制项标成 `kFallback`。

作用域是 **(clipSlot, endSlot]**，两头都不对称：

| 位置 | 是否被裁 | 出处 |
|---|---|---|
| 裁剪附件**自己**那个槽位 | **不裁**（它压根不画） | `SpineMesh.h:153-159`：命中 clipping ⇒ `clipStart()` 然后 `continue` |
| 它**之后**的槽位 | 裁 | `:189-197` `isClipping()` 为真就走 `clipTriangles()` |
| **endSlot** | **含在内** | `:257` 槽位收尾调 `clipEnd(slot)`，而它只在 `_clipAttachment->_endSlot == &slot._data` 时收尾（`SkeletonClipping.cpp:72-76`） |
| endSlot 之后 | 不裁 | 同上 |

三条容易漏、漏了就**静默**错的规则：

1. **裁剪附件那个槽位被 `continue` 掉了**，连槽位收尾的 `clipEnd(*slot)` 也一起跳过了
   （`:159` 的 continue 在 `:257` 之前）。⇒ **end 写成"自己或更靠前的槽位"时，
   `active_end == s` 永远不成立**，这次裁剪只能靠整条绘制指令末尾那次无条件
   `clipEnd()` 收尾（`:259`）⇒ **一路裁到最后一个槽位**，且不报任何错。
2. **嵌套裁剪被忽略**：已经有一个裁剪在跑时 `clipStart()` 直接返回 0
   （`SkeletonClipping.cpp:49-51`）⇒ 后面那条裁剪**永远开不起来**，
   哪怕它的 endSlot 更靠后，也不会在前者结束后补上。
   ⇒ 作用域**不能**用"存在某条 clip 覆盖这个槽"的宽松判定，必须把槽位序列走一遍。
3. **作用域是逐皮肤的**：裁剪附件本身是某皮肤下某槽位的附件，3.8 同时只有一个皮肤生效
   （容器里 `skin_id` 是绘制项的键之一）⇒ 换到没有它的皮肤时作用域不存在。

#### 导入期从两种输入里取作用域（两者**不对称**）

| 输入 | 结束槽位的表示 | 参考实现 | 本导入器 |
|---|---|---|---|
| json | 槽位**名**（`"end"`） | `findSlot()` 的返回值**不检查**（`SkeletonJson.cpp:631-635`）⇒ 槽位名写错 / 缺失时 `_endSlot` 静默变 NULL ⇒ 作用域从"到某槽"变成"到末尾" | **报错**（报出附件名 + 槽位名）；**缺 `"end"` 则是合法的** ⇒ 记为"裁到最后一个槽位" |
| `.skel` | 槽位**下标**（varint，**在 vertexCount 之前**） | 有显式边界检查，越界报 "parse clip attachment failed!"（`SkeletonBinary.cpp:641-645`） | 同一条检查，`< 0` 也显式判（不依赖 `int >= size_t` 的整型提升方向） |

#### 容器里存什么

**作用域本身不落盘。** 它已经完整表达成"作用域内绘制项的 `kind = kFallback`"，
运行时不需要再算一遍。

**裁剪多边形要落盘**（chunk 9 `clip_data`，v6 起）。它和 deform **不同**：
deform 是逐帧动画数据（来自时间轴，永远不该烘焙），而裁剪多边形是
**静态资产几何** —— 局部坐标 + 骨骼绑定，和 mesh 顶点同源
（`ClippingAttachment : VertexAttachment`），不烘焙的话运行时想"只靠容器渲染、
不再持有 spine 源数据"就不成立。

```
ClipPolygon                  20 B（chunk 9，一条/裁剪附件；v6–v14 是 12 B）
  slot_index    u16   @0   裁剪附件所在槽位
  skin_id       u16   @2   作用域是逐皮肤的
  vertex_format u8    @4   kRigid / kWeighted
  _pad          u8    @5
  vertex_count  u16   @6
  vertex_offset u32   @8   **段内顶点下标**，不是字节偏移（见 §3.1 的 stride）
  name_offset   u32   @12  v15：附件名（字符串表第 9 组，与 chunk 9 一一对应）
  end_slot      u16   @16  v15：结束槽位（含）；0xFFFF = 没有（裁到绘制顺序末尾）
  _pad2         u16   @18
```

> **v15 起"作用域不落盘"不再够用**：烘焙期按 setup 附件 + setup 绘制顺序模拟出的作用域，
> 在 attachment / drawOrder 时间轴动了之后就不成立（实测 OSGame 有动画里换裁剪附件、改绘制顺序的）。
> 运行时改为**每帧按当前状态重跑状态机**（见下面"v15：运行时裁剪"），所以 chunk 9 补上名字与结束槽位。
> 绘制项的 `kind` / `clip_index` 仍写（setup 时的结论，诊断用），渲染器不再依赖它们。

⚠️ **多边形顶点复用主顶点缓冲**（chunk 3），不另开一块：`ClippingAttachment` 就是
`VertexAttachment`，编码方式与 mesh 顶点**完全相同**，也走同一个 `ExactBlobPool`
逐字节去重 ⇒ header 不需要新增字段，仍是 64 B。
被裁的绘制项通过 `clip_index` 指到这张表的行号；**没被裁时必须是 `kNoClipIndex = 0xFFFF`，
不能是 0 —— 0 是一条真实存在的多边形**，填 0 会让运行时拿第一条多边形去裁根本不该裁的项，
而且不报错。

⚠️ `vertex_offset` 的单位是**顶点下标**不是字节。只有一条多边形时它落在段首，
`0 × stride` 还是 0，两种解释逐字节相同 ⇒ 写错了测不出来；验收必须用到**第二条**
多边形（偏移非 0）才能钉住（见 §12 第 12 项）。

⚠️ **`kHasDeform` 只由 deform 点亮，不由裁剪点亮。** 两个位都表示"有绘制项不完全走
GPU 路径"，但语义与**代价**都不同：
`kHasDeform` = 顶点**逐帧**变，但变的只是**蒙皮之前的局部空间加数**（`VertexAttachment.cpp:122-123`
`vx = (*vertices)[b] + (*deformArray)[f]`）⇒ v7 起走 **deform 增量**，共享 VB 不动、蒙皮仍在 GPU
（`doc/GPU_SPINE_V7_ANIMATION.md` §5.2；**不再是**全量 CPU 动态 VB）；
裁剪则是 `SkeletonClipping::clipTriangles()` 重写顶点/三角形/UV ⇒ 只能 CPU 生成动态 VB，
且只在裁剪多边形动的时候才需要重算。合成一个位，运行时会为纯裁剪的资产
每帧重算一份根本没变的顶点。

---

## 8. 混合模式与预乘 Alpha

`BlendMode.h:35-38`：`BlendMode_Normal = 0, Additive, Multiply, Screen`。

Spine 图集默认是 **PMA（预乘 Alpha）**，对应 Unity 侧 `Blend One OneMinusSrcAlpha`。
本规格规定：**图集按 PMA 导入，混合模式按上表映射**，`tint` 在 shader 里乘到 PMA 颜色上。

| Spine | ZEngine / ShaderLab | Unity |
|---|---|---|
| Normal (PMA) | `Blend One OneMinusSrcAlpha` | 同 |
| Additive | `Blend One One` | 同 |
| Multiply | `Blend DstColor Zero` | 同 |
| Screen | `Blend One OneMinusSrcColor` | 同 |

混合模式不同 ⇒ 必须分批（合批键的一部分）。

---

## 9. 容器二进制布局

```
Header (64 B)
  0x00  magic            u32   'ZSPG'  (0x5A535047)
  0x04  version          u32   本规格版本，当前 11
                               v2 = 加了 skin 维度（§9.1）
                               v3 = UV 拆成独立通道 + chunk id 重排（§3.2、§14.2）
                               v4 = deform 参与烘焙（§7.1）—— 布局没变，
                                    但带 FFD 的绘制项 kind 从 kStatic 变成 kFallback
                               v5 = 裁剪作用域参与烘焙（§7.2）—— 布局没变，
                                    但作用域内绘制项的 kind 从 kStatic 变成 kFallback
                               v6 = 裁剪多边形参与烘焙（§7.2）—— 绘制项 24 B → 28 B
                                    （新增 clip_index），chunk 9 起真正写数据
                               v7 = 蒙皮数据搬出顶点（§3.1）—— weighted 顶点
                                    48 B → 8 B 引用，新增 chunk 21 / 22；
                                    顶点数 / 绘制项 / header 大小都没变，但旧容器
                                    用新读端会把"引用"当 bindPos ⇒ 形状全错
                               v8 = 骨骼树进容器（§3.4）—— 新增 chunk 10，36 B/骨；
                                    header / 绘制项 / 顶点布局都没动，但没有它
                                    动画就无从解算（时间轴是相对 setup pose 的增量）
                               v9 = 动画进容器（§3.5）—— 新增 chunk 11-16；
                                    header 与既有 chunk 全都没动（动画数量由 chunk 11
                                    的大小反推）；flags 多一位 kHasEventTimeline
                               v10 = 槽位 setup 进容器（§3.6）—— 新增 chunk 17，40 B/槽；
                                    header 与既有 chunk 全都没动，字符串块多一组
                                    「槽位 setup 附件名」（slot_count 条）。
                                    之前容器里**没有颜色值**（只有一个 uses_tint_black
                                    布尔），而 color / twoColor 时间轴是相对 setup 色的
                                    ⇒ 缺了它就只能默认纯白，而纯白与 setup 白在多数资产
                                    上完全看不出区别（与 v8 / v9 同一类坑）
                               v11 = 约束进容器（§3.7）—— 新增 chunk 18 约束 setup、
                                    19 执行顺序、20 PathAttachment 几何；
                                    header 与既有 chunk 全都没动，字符串块多一组
                                    「path 附件名」（chunk 20 的条数，排在动画附件名
                                    **前面**）。约束**每帧都跑**、与时间轴无关，
                                    实测 481 / 781（61.6%）个资产带约束
                                    ⇒ 缺了它们的表现是"姿势不动"，与 v8 / v9 / v10
                                    是同一类坑（"没有 X"本身不报错）
                               v12 = deform 时间轴可以指向 **path 附件**：`target` 最高位
                                    （`kDeformTargetPathBit = 0x8000`）置 1 时，低 15 位是
                                    chunk 20 的下标（否则仍是附件记录下标）。**字节布局一个
                                    都没变**，变的是语义：v11 烘焙期把它当"不渲染、无效果"
                                    跳过，但 path 的 deform 会改 path 约束摆出来的骨骼
                                    （VertexAttachment.cpp:69-70）。读端校验下标落在
                                    chunk 20 内、每帧 start+count ≤ deform 长度
                                    （rigid = 顶点 float 数；weighted = float 数 / 3 × 2）
                               v13 = 渲染侧前置修正，**字节布局一个都没变**：
                                    ① `DrawItem::uv_offset` 改落 `uv_unit - vertex_base`
                                    （§3.2，v12 的公式只在 vertex_base == 0 时对）；
                                    ② `blend_mode` 落槽位的真实值（v12 恒 normal；实测
                                    additive 30 244 / multiply 78 / screen 50 项）；
                                    ③ `atlas_page` 落 region 的真实页（v12 恒 0）。
                                    读端新增逐项 `uv_offset + index < uv_count` 校验
                               v14 = 绘制项 28 → 32 B：`vertex_base` / `deform_parent` / `vertex_count`
                                    （§4），GPU mesh deform（S6，§7.1）的前提。同时修了一个
                                    导入期的静默错：附件重排时 linked mesh 的 `parent_index`
                                    没跟着改写（v13 及以前只在重排前用过它，所以没暴露；
                                    落进 deform_parent 后 OSGame 73 个资产被读端校验抓到）
                               v15 = 运行时裁剪（§15）：绘制项 @23 的 pad 变 `item_flags`
                                    （bit0 = region），chunk 9 补 `name_offset` /
                                    `end_slot`，裁剪附件自己的 deform 进容器（target
                                    次高位 0x4000）。裁剪作用域改**每帧**按当前绘制
                                    顺序重算（烘焙期的作用域在动画里不成立）
                               v16 = 用户事件进容器（§16）：新增 chunk 23（事件定义
                                    `SpineGpuEventData` 28 B/条 + 自带字符串池）；
                                    事件时间轴 type 7 第一次真正进 chunk 12，帧载荷
                                    = `SpineGpuEventKey` 24 B/帧。header 与既有 chunk
                                    布局**一个字节都没动**，变的是两处语义：
                                    ① 动画 duration 现在把最后一个事件的 time 算进去
                                      （v15 及以前不烘事件 ⇒ 少算，complete 时机错）；
                                    ② 混合期 totalAlpha / timelineMode 多一条事件
                                      时间轴参与
                               v17 = **骨骼权重 unorm8 → unorm16**（§3.1）：chunk 22 每条仍是
                                    4 B（`boneIdx u16 @0` + `weight unorm16 @2`，原 @3 的 pad
                                    被吃掉），**布局字节数没变、语义变** —— v16 读端会把高字节
                                    当 pad、低字节当 u8 权重 ⇒ 权重全错、**不报错**。
                                    每个权重顶点的权重和恰好 65535（原来 255），读端逐顶点校验。
                                    跨端影响：`spine_skinning.h::SpineInfluenceWeight`、
                                    `SpineGpuDecodeWeight()`、C# `IbWeight` 宽度、两处手搓夹具
                                    （`SpineSkinningParityTest` / `SpineGpuDx12RasterTest` 的
                                    u8 值要 ×257 迁移到 unorm16）
  0x08  spine_runtime    u32   产生它的 spine-cpp 版本（3.8.xx → 30800）
        ⚠️ 必须记录：3.8 的 _bones 是 u16，4.x 是 int，布局不同
  0x0C  flags            u32   kIndex32 / kHasDrawOrderTimeline / kUsesTintBlack / kHasClipping
                               / kHasDeform（有绘制项因 FFD 走了 CPU fallback，见 §7.1）
                               / kHasEventTimeline（v9：有事件时间轴。v9~v15 事件
                                 **不烘焙**，但"有却不记"等于静默丢弃 ⇒ 用一位显式
                                 说出来；v16 起事件真的进 chunk 23 + 时间轴 type 7，
                                 这一位仍在，语义不变）
  0x10  bone_count       u32
  0x14  slot_count       u32
  0x18  draw_item_count  u32
  0x1C  vertex_count     u32
  0x20  index_count      u32
  0x24  atlas_page_count u32
  0x28  bone_stride      u32   24 或 32
  0x2C  chunk_count      u32   由 writer 填（内存中未烘焙的 asset 里是 0）
  0x30  rigid_vertex_count      u32
  0x34  weighted_segment_offset u32   weighted 段在 vertex_buffer 里的字节起点，8 B 对齐
  0x38  weighted_vertex_count   u32
  0x3C  skin_count      u32   （v1 这里是 reserved）

ChunkTable  (chunk_count × 16 B)  { id u32, offset u32, size u32, _pad u32 }
Chunks:
  id=1  strings        骨骼名 → 皮肤名 → 槽位名 → **槽位 setup 附件名（v10）** →
                       附件名 → 图集页名 → **动画名** → **path 附件名（v11）** →
                       **动画附件名**（v9），每项 UTF-8 且 '\0' 结尾。
                       ⚠️ 第 4 组**每条槽位一条，空名也占一条** —— 不占位就没法按
                       slot_count 切分，后面几组会整体错位（不报错，只是名字错）。
                       ⚠️ 第 8 组（path 附件名）按 chunk 20 的条数切分
                       （**一一对应，不去重**），且**必须排在动画附件名前面** ——
                       "动画附件名"只按偏移访问、没有计数 ⇒ 排在它后面就切不出来。
  id=2  slot_bone      每槽位的骨骼下标（u16 × slot_count）—— rigid 用
  id=10 bone_setup     SpineGpuBone[]，36 B/骨（§3.4，v8）
                       { x, y, rotation, scale_x, scale_y, shear_x, shear_y, length,
                         parent i16, transform_mode u8, _pad u8 }
                       下标 = header.bone_count 的序，也 = 骨骼名的序号，
                       也 = 影响流 bone_idx 引用的同一个下标 ⇒ **三处必须同一空间**
  id=3  vertex_buffer  按 vertex_format 分段，段首 8 B 对齐（**不含 UV**）
  id=4  uv_buffer      float2 × uv_count，8 B/条，段首 8 B 对齐（§3.2）
  id=5  index_buffer   uint16（或 flags 打开时 uint32）
  id=6  draw_items     DrawItem[]，32 B/项（含 uv_offset + skin_id + clip_index；v14 + vertex_base / deform_parent / vertex_count）
  id=7  attachments    (slot_index u16, attachment_id u16, skin_id u16, _pad u16) × attachment_count
                       —— 8 B/项，换装查的就是这张表
  id=8  atlas_pages    图集页路径，每项 '\0' 结尾
  id=9  clip_data      ClipPolygon[]，12 B/条（§7.2）；**顶点本身在 chunk 3 里**。
                       没有裁剪时这个 chunk **根本不写**（不是写个空块）
  id=11 anim_dir        SpineGpuAnim[]，20 B/条（§3.5，v9）
  id=12 timelines       SpineGpuTimeline[]，32 B/条（§3.5，v9）—— 全局一张表
  id=13 frames          f32[]，帧数据平面数组（§3.5，v9）
  id=14 curve_types     u8[]，每段一个（§3.5，v9）
  id=15 beziers         SpineGpuBezier[]，16 B/条（§3.5，v9）
  id=16 timeline_payload 字节：attachment 名 / deform 稀疏帧 / drawOrder 偏移（§3.5，v9）
  id=17 slot_setup     SpineGpuSlot[]，40 B/槽（§3.6，v10）
                       { color rgba f32×4 @0, dark rgba f32×4 @16,
                         setup_attachment_name_offset u32 @32, flags u32 @36 }
                       下标 = header.slot_count 的序 = 槽位名的序号 =
                       时间轴 Target（attachment / color / twoColor）的同一个空间
                       ⇒ **三处必须同一空间**（分叉了不报错，只是颜色挂到别的槽位上）
  id=18 constraint_setup 块头 16 B（ik/tf/path/bone_pool 四个 count）+ 三段定长数组
                       + 尾部共用的 u16 骨骼池（§3.7.1，v11）。
                       没有约束时这个 chunk **根本不写**（不是写个空块）
  id=19 update_order   块头 8 B（step/reset 两个 count）+ SpineGpuUpdateStep[] 4 B/步
                       + uint16 reset[]（§3.7.2，v11）。同样只在有约束时写。
                       ⚠️ **有 chunk 18 却没有 chunk 19 ⇒ 必须报错**
  id=20 path_attachments 块头 16 B（att/bone/vertex/length 四个 count）
                       + SpineGpuPathAttachment[] 40 B/条 + u16 骨骼池
                       + f32 顶点池 + f32 lengths 池（§3.7.3，v11）
  id=21 influence_bind bindPos f32×2 × 影响条目数，8 B/条（§3.1，v7）
  id=22 influence_bone (boneIdx u16, weight unorm16) × 影响条目数，4 B/条（v7；v17：pad 变成权重高字节）
                       ⚠️ 21 与 22 **共用同一个条目下标**（= 权重顶点的
                       `influence_offset`）⇒ 读端必须校验两条的条目数**相等**，
                       对不上就报错（配错了对不报错，只是每个影响取到别人的数据）。
                       id 从 21 起：10..20 留给动画阶段（骨骼树 / 时间轴 / path 几何）。
  id=23 events        块头 16 B（event / string / blob 三个 count + reserved）
                       + SpineGpuEventData[] 28 B/条
                       + u32[] 每条字符串在 blob 里的字节偏移
                       + blob（UTF-8，每条 '\0' 结尾），整体补齐到 4 B（§16，v16）
                       **没有事件时这个 chunk 根本不写**（与 chunk 9 / 18 / 19 同规矩）
                       ⚠️ 字符串池**第 0 条恒为空串**（string / audio 的"缺省"，
                          引用 0 即可，不用哨兵），且是**下标**引用，不是 chunk 1
                          那种字节偏移
```

> ⚠️ **chunk id 在 v3 重排过**：`uv_buffer` 插在 `vertex_buffer` 与 `index_buffer` 之间，
> 其后所有 id 顺移一位。读端按 **id** 找 chunk，**不许按下标** —— v2 的容器用 v3 读端
> 读会拿到 `index_buffer` 当 UV，静默错位。这也正是 `kFormatVersion` 升到 3 的原因。
>
> ⚠️ **文件里 chunk 的顺序也不是 id 顺序**：写端按
> `1, 2, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 3, 21, 22, 4, 5, 9, 6, 7, 8, 23` 追加
> （骨骼 setup 紧挨 slot_bone；v9 的动画块 11-16 紧跟其后、v10 的槽位块 17 再跟其后、
> v11 的约束块 18-20 又跟其后；影响流紧跟
> 顶点块；9 夹在 5 与 6 之间是
> 历史顺序）。换了顺序文件照样能读，但字节不再逐字节相同 ⇒ 跨语言对拍（§12）立刻红，
> 所以 C# 端必须照抄这个顺序（~~见 `SpineGpuContainerIo.cs`~~ **已删**，见下）。
>
> ⚠️ 2026-10-30：C# 容器镜像（`Runtime/CSharp/`）已整目录删除，跨语言对拍
> （§12 的 `SpineGpuCSharpParityTest`）一并移除 —— Unity 侧的导入器改成引擎内
> C++（`Modules/GPUSpine/GPUSpineBakedAsset` 直接调 core 的
> `BakeSpineGpuAsset` / `ReadSpineGpuContainer`），"复刻一份再对拍"这层需求消失。
> 与 GPUParticle 同一条理由（见 `Libraries/GPUParticle/doc/GPU_PARTICLE_DESIGN.md` §六）。
> 顺序本身仍然必须照抄 —— 现在照抄的是"另一个引擎的 C++ 读端"，而它与本端
> 是同一个子模块的同一份代码，不需要靠对拍去发现漂移。

### 9.1 skin 维度（v2）

容器里的"一个附件"= **(皮肤, 槽位, 附件名)** 三元组，不再跨皮肤合并。
换装的查询是：给定 `(skin_id, slot_index)` → `attachment_id` → 绘制项。

- **`skin_id` 0 恒为 default 皮肤。** 二进制格式的 default 皮肤就是先读的那一个
  （`SkeletonBinary.cpp:276`，名字写死 `"default"`）；编辑器导出的 json 里 default 也总排第一。
  但**导入器会显式检查**：名为 `default` 的皮肤若不在首位就**报错**，绝不静默挪位 ——
  挪了默认外观就变成另一个皮肤，而外观错没有任何报错。
- 字符串表里皮肤名紧跟骨骼名之后，读端靠 `header.skin_count` 切分。
- 代价：**同一个 (槽位, 附件名) 在 N 个皮肤下仍是 N 份附件记录**，但 v3 之后
  **几何不再跟着翻 N 倍** —— 顶点逐字节去重，跨皮肤只多一份 UV（§3.2、§14.2）。
  剩下的 N 份只是绘制项 + 附件记录 + UV 条目，是"能表达换装"的必要成本，不是浪费。

**为什么 header 里要有 0x30..0x38 这三个字段**：顶点缓冲是 rigid 段 + weighted 段拼起来的，
光有 `vertex_count` 和 `draw_items` 无法反推出分界点 —— 读回来就切不开两段，
而 `SpineRigidVertex`(8 B) 与 `SpineWeightedVertex`(8 B) 虽然步长现在相同、但**含义不同**
（一个是 bindPos，一个是"影响流引用"），切错就全错。
所以段信息必须由 header 显式给出，不能靠消费端重算。

布局规则：
- 所有 chunk 起点 4 B 对齐；`vertex_buffer`(id=3)、`influence_bind`(id=21) 与 `uv_buffer`(id=4)
  起点 **8 B 对齐**（§3.1 的 `Load2` 前提；bindPos 与 UV 都是 `float2`，同样要 8 B）。
- 对齐填充插在 chunk 之间，**因此容器总长 ≠ header + 表 + 净载荷之和**。
  要校验容器，应校验 **chunk 表里的 `size` 之和**（与填充无关的精确不变量），
  而不是写死一个总字节数 —— 后者会在对齐规则一变时就假失败。
- 最后一个 chunk 的 `offset + size` 必须正好等于文件长度：不留尾部冗余，也没被截断。

---

## 10. 精度与坐标系

- **骨骼表、绑定坐标、UV 一律 `f32`。默认不启用 f16。**
  理由：本项目已在别处吃过 fp16 的亏；Spine 坐标量级是像素（可达 1e3~1e4），
  图集可达 4096²，f16 在 2048 附近就有 ~2 px 的 UV 误差。
  （如有明确收益，可作为 `flags` 里的可选变体另议，不在 v0.1 范围。）
- 坐标系：**骨架空间，y 向上，1 单位 = 1 像素**（Spine 原生约定）。
  不要为了减少一次运算而在导入期翻转 y —— 那会让 ZEngine 与 Unity 的 UV 对不上。
- 顶点最终位置 = `root.apply(skinned)`，再乘视图投影。

---

## 11. 版本与兼容

- `version` 变化 ⇒ 导入器必须拒绝旧容器并给出明确错误，不做静默兼容。
- **v7 → v13 已落地**：v7 是影响流（chunk 21 / 22）+ 逐顶点变长影响数；
  **v8 追加骨骼树（chunk 10，§3.4）**；**v9 追加动画（chunk 11-16，§3.5）**；
  **v10 追加槽位 setup（chunk 17，§3.6）**；
  **v11 追加约束 setup / 执行顺序 / PathAttachment（chunk 18 / 19 / 20，§3.7）**；
  **v12 让 deform 时间轴可以指向 path 附件（`target` 最高位，布局不变、语义变）**；
  **v13 修正绘制项的 `uv_offset` / `blend_mode` / `atlas_page`（布局不变、语义变，§3.2 / §4）**；
  **v14 绘制项 32 B（+ vertex_base / deform_parent / vertex_count），GPU mesh deform（S6，§7.1）**；
  **v15 运行时裁剪（§15）**；**v16 用户事件进容器（chunk 23 + 时间轴 type 7，§16）**。
  ⚠️ v14 容器被 v15 读端读会得到 **"region 贴图转 90°"**（item_flags bit0 不在旧容器里，
  按 0 解 = 非 region ⇒ 喂 `clipTriangles` 的顶点顺序不对），而"转 90°"本身不报错
  —— 是同类的第五个坑。
  ⚠️ v15 容器被 v16 读端读会得到 **"事件全丢"**（chunk 23 不存在），且**动画 duration
  偏短**（末尾事件的时间没算进去）⇒ complete 时机错、不报错 —— 是同类的第六个坑。
  约束解算（S4b）已收尾：781 / 781 逐骨骼对拍通过，解算器自写、仍在 CPU。
  渲染接入见 **§15**。
  字段设计与转录 gap 分析见 **`doc/GPU_SPINE_V7_ANIMATION.md`**。
  v7 / v8 / v9 / v10 **都不动** header（仍 64 B）、不动 chunk 1..9、不动 §5 每帧实例数据、
  不动 §6 shader 数学；新增的计数一律从 chunk 表推导（`size / stride`），不进 header
  （v9 的动画数量就是从 chunk 11 的大小反推的）。
  ⚠️ v8 容器被 v9 读端读会得到 **0 个动画**（chunk 11 不存在），而"没有动画"本身
  不报错 —— 与 chunk 10 那一类是同一个坑，所以仍然是不兼容升级。
  ⚠️ v9 容器被 v10 读端读会得到 **"所有槽位纯白 + 无暗色"**（chunk 17 不存在），
  而"纯白"本身不报错 —— 实测 **139 / 781 个资产**真的有非纯白 setup 色，
  是同类的第三个坑。
  ⚠️ v10 容器被 v11 读端读会得到 **"0 条约束"**（chunk 18 / 19 / 20 都不存在），
  而"没有约束"本身不报错 —— 实测 **481 / 781 个资产（61.6%）真的有约束**，
  缺了它们的表现只是**姿势不动**，是同类的第四个坑。
  ⇒ **四个坑的共性**：新增的语义在旧容器里都表现为"某个合理的默认值"，
  逐字节读得通、读得合法，只是结果静默错。所以每次加 chunk 都必须连带 bump
  `version` 并让读端**硬拒**，不能"尽力而为"。
- `spine_runtime` 不匹配 ⇒ 警告但允许（布局差异只在 `_bones` 宽度，导入器已按 3.8 读；
  4.x 资产需要另一条导入路径，**不在本版本范围**）。
- 任何"导入器静默调整数据"的行为（截断影响数、重排顶点、自动降精度）都**必须报错**，
  不允许悄悄做。

---

## 11.1 两端各有一份实现（C++ / C#）

格式的**权威定义**在 `SpineGpuFormat.h`（C++）。C# 侧是**照抄**，不是"参考"：

| 端 | 位置 |
|---|---|
| C++ | `Function/Render/Spine/SpineGpuFormat.h`（布局 + static_assert）<br>`SpineGpuImporter.cpp`（导入 + `Write/ReadSpineGpuContainer`） |
| C# | ~~`Runtime/CSharp/`（`SpineGpuFormat.cs` / `SpineGpuContainerIo.cs`）~~ **2026-10-30 已删** |
| Unity（替代） | 引擎内 C++：`Modules/GPUSpine/GPUSpineBakedAsset.{h,cpp}`，core 来自同一个子模块 `External/GPUSpine` |

C++ 每个偏移都有 `static_assert` 钉着（布局错一个字节不报错，只是形状静默错）。
C# **没有编译期 `offsetof`**，所以换三道关：

1. 偏移写成**命名常量**（`SpineGpuOffsets`），读写两端共用同一组 ⇒ 不可能半边漂。
2. `SpineGpuLayout.Validate()` 在启动时跑一遍常量自洽检查 —— 等价于 static_assert 那一层。
3. 真正的钉子在对拍：C++ 写的字节，C# 读回来再写一遍必须**逐字节相同**。

⚠️ **round-trip 只能抓"大小"漂，抓不到"解读"漂。** 读与写用同一组常量时，
把 `clip_index` 与 `pad2` 的偏移对调，字节照样自洽（因为对称）。
⇒ 必须有**语义断言**（哪些项被裁、多边形顶点落在哪、骨骼索引是不是 0/1）。

跨语言对拍怎么跑：

```
cmake -B build -DZENGINE_BUILD_SPINE_IMPORT_TEST=ON
cmake --build build --target SpineGpuImportTest --config Debug
ctest -C Debug -R SpineGpuCSharpParityTest
```

`SpineGpuImportTest --dump <dir>` 把夹具容器落成 `.zspg`（18 个，覆盖
baseline / linked mesh / 双皮肤 / deform / 裁剪的三种 end / 嵌套 / weighted 多边形 /
nonessential 开与关），C# 端读回来再写一遍比对。
没装 dotnet 时这条测试不注册（它是**加码**，不是构建的前置条件）。

## 11.2 容器由 C++ 唯一产出（`ZEditor -batchmode -suite=SpineCook`）

既然 C# 不做导入器，产出的入口就是一个：

```
ZEditor -batchmode -suite=SpineCook <输入> [-o <输出>] [-atlas <图集>] [--recursive|-r] [-h]
```

* **输入**：一个 `.json` / `.skel`，或一个目录（加 `--recursive` 连子目录一起走）。
* **解析器按扩展名选**，不"看内容像不像 json" —— `.skel` 既没魔数也没长度字段，
  猜错就是越界读。
* **图集**：省略则取同名 `.atlas`；找不到**报错**，不"尽力而为"（用错图集算出的
  UV 是静默错的，图集带裁剪时偏一点且不报任何错）。
* **目录 → 目录**保持相对结构。⚠️ 判据是"输入是目录"或"输出已存在且是目录"，
  **不能**只看 `is_directory(输出)`：输出目录第一次跑时还不存在，判成文件就会让
  每个骨架写同一条路径、互相覆盖，而覆盖在批处理里退出码还是 0。
* **有失败就退出码非 0**（"退出码 0 但少导了两个"是最难查的事故）；
  不认识的开关直接拒绝，不静默吞掉。

为什么走 `-batchmode` 而不是编辑器里的菜单：编辑器那套 `START_SYSTEM()` 会拉起
RHI、世界、资源系统，跑一个"读文件 → 算 → 写文件"的烘焙器全都不需要它们。
分流点在 `Main.cpp` 里 `RegisterRuntime()` **之前**（`Editor/ToolSuite/ToolSuite.h`
的注释写了原因）。

---

## 12. 如何证明两端一致（验收清单）

1. **CPU ↔ GPU 对拍**：固定骨架 + 固定时间 `t`，
   把 `computeWorldVertices` 的结果与 GPU 回读结果逐顶点比较。
   - 逐位一致：需关 FMA 收缩（§6 约束 3）
   - 否则：容差 `1e-3` 相对误差，且坐标量级 < 1e4
2. **ZEngine ↔ Unity 对拍**：同一个容器、同一 `t`，两端截图 / 回读比对。
   骨架与图集必须同源（同一份 `.skel` + `.atlas`）。
3. **回归夹具**：至少一份含 **Region + 刚性 Mesh + 蒙皮 Mesh + tintBlack + 四种混合模式**
   的骨架，作为永久对拍资产。
4. **导入器负向用例**：`n > 255` 的顶点必须报错；`weights` 和不为 255 必须报错；
   认不出的 `transform` 串必须报错（参考实现静默退回 normal）；
   父骨骼不存在必须报错（不能静默当成根骨骼）；`transform_mode > 4` 必须报错。
5. **骨骼 setup 的语义断言**（§3.4）：夹具里两根骨骼必须带**非默认**的
   x/y/rotation/scale/shear/length，其中一根的 `parent` **非 -1**，
   且两根的 `transform_mode` 不同 —— 只比"条数对不对"抓不到解读漂。
6. **两种输入必须对拍**：`.skel`（编辑器默认导出）与 json 是两条独立解析路径，
   同一副骨架经两条路径 Bake 后必须产出**逐字节相同**的容器 ——
   v8 起这条覆盖了骨骼 setup、v9 起又覆盖动画（11 类时间轴 + 曲线）：
   二进制的字段顺序与 json 的默认值必须殊途同归。
   ⚠️ 为此动画时间轴在 Bake 时按 **(type, target) 升序稳定排序** —— 两条路径的
   条目顺序本来就不一样（json 取决于文件里键的书写顺序），不规范化就对不上。
另：对 `.skel` 夹具的**每个真前缀**都调一次解析，必须全部失败 —— 它既没魔数也没长度字段，
截断是最容易触发越界读的场景。
6. **换装必须验两个皮肤**：同一副骨架里两个皮肤在同一槽位放同名的附件（几何不同），
   两份都必须留在容器里，且能按 `(skin_id, slot_index)` 查回各自的 `attachment_id`。
   （v1 会把它们合并成一份 ⇒ 换装静默失效，这条就是为它设的回归。）
7. **容器必须可重入**：读回来的资产再序列化一次，字节必须与原来**逐字节相同** ——
   否则 C# 端读到的就不是同一个东西（指针 / 地址 / 未初始化填充都会在这里露出来）。
8. **几何共享必须被钉住**：含 linked mesh 的骨架，Bake 后**顶点数必须等于父附件的顶点数**
   （而不是翻倍），同时两个绘制项的 `uv_offset` 不同、UV 值不同。
   只断言"能画出来"不够 —— 去重一旦退化成复制，画出来照样是对的，内存却悄悄翻倍。
   配套断言：**`uv_count ≠ vertex_count`**，防止有人拿 `vertex_count` 去算 UV 数组长度。
9. **deform 必须逐项回退，不能整体回退**：带 FFD 的骨架里，**只有**被时间轴命中的绘制项
   是 `kFallback`，同一个骨架里的其它项必须仍是 `kStatic`；开了 `inheritDeform` 的
   linked mesh 跟着回退，关了的**不**跟着回退（§7.1）。
   只断言"deform 存在 ⇒ 有 fallback"不够 —— 那会把"整个资产回退"也放过。
10. **裁剪作用域的**两头**都要钉住**：4 槽位夹具（`s0` 在裁剪之前、`s1` 是裁剪附件自己、
   `s2` 在作用域内、`s3` 是边界槽位）里 ——
   * `end = s2` ⇒ `s2` 回退、**`s3` 不回退**（结束槽位是**含**在内的）
   * `end = s1`（自己）/ `end = s0`（更靠前）⇒ `s2`、`s3` **都**回退（收尾检查被 `continue` 跳过了）
   * 缺 `"end"` ⇒ 一路裁到最后一个槽位
   * 裁剪附件自己那个槽位**没有**绘制项
   且 **只有裁剪时不置 `kHasDeform`**（§7.2）。
   只断言"有裁剪 ⇒ 有 fallback"不够 —— 那既放过"整个资产回退"，也放过"结束槽位开/闭写反"。
11. **嵌套裁剪的判据必须是"最后那个槽位"**：5 槽位夹具（`s1`=clipA→`s4`，
   `s2`=clipB→`s3` 套在 clipA 里）里 ——
   * 正确（clipB 被忽略）⇒ `s3`、`s4` 都由 **clipA** 裁 ⇒ `s4` 是 `kFallback`
   * 错误（clipB 生效并终结 clipA）⇒ clipB 在 `s3` 就收尾 ⇒ **`s4` 变成 `kStatic`**
   ⚠️ 只断言 `s3` 不够 —— 两种实现都会让 `s3` 被裁，只有 `s4` 分得开。
12. **裁剪多边形必须落盘，且 `clip_index` / `vertex_offset` 的语义要钉住**：
   * 没有裁剪时 chunk 9 **根本不存在**（不是写个空块），且每个绘制项都是 `kNoClipIndex`
   * 被裁的项 `clip_index` 指到那条多边形；**没被裁的必须是 `kNoClipIndex` 而不是 0**
     —— 0 是一条真实存在的多边形，填 0 会让运行时裁错东西且不报错
   * 多边形顶点复用主顶点缓冲（chunk 3），逐点与 json/`.skel` 里的多边形相同
   * **`vertex_offset` 的单位是顶点下标不是字节** ⇒ 必须用到**第二条**多边形（偏移非 0）
     才能测出来；只有一条时它在段首，`0 × stride` 与 `0` 逐字节相同，写错也测不出来
   * 二进制的 weighted 判据是**显式 bool**，json 只能靠"数组长度 ≠ vertexCount×2"猜
     （没有 uvs 可比对）⇒ 两种输入必须对到同一个结果；json 缺 `vertexCount` 必须报错
     （参考实现会把它误判成 weighted ⇒ 静默解析成垃圾骨骼索引）
   * **影响流（chunk 21 / 22）** 两边条目数必须相等；每个权重顶点引用的区间必须落在流里
     （读端显式校验，越界一律报错 —— 不报错的话 shader 只会读到别人的 bindPos）
   * ⚠️ **必须有权重顶点的 `influence_offset` 非 0**：全是 0 时"写错成恒 0"测不出来
     （跟裁剪多边形 `vertex_offset` 那条是同一个道理）
   * 影响数 **> 4 的顶点必须能被烘焙**（v7 的目的就是这个）——夹具里放了 5 个影响的顶点
   * `influence_count` 个 unorm16 权重的和必须**恰好 65535**（v17；v16 及以前 255）——
     把旧容器的 u8 权重当 u16 读，或写端量化错，都会在这里被抓到
13. **必须跨语言对拍，不能两端各自自测**（§11.1）：C++ 落的 `.zspg`，C# 读回来再写一遍
    必须**逐字节相同**；同时要有语义断言，因为 round-trip 抓不到"解读"漂。
14. **槽位 setup 的语义断言**（§3.6，v10）—— 三条都是"错了不报错"的：
    * **夹具必须有非默认色的槽位**：全是纯白的话，"写端根本没写颜色"也会被判通过
      （读回来还是纯白）。⇒ 基线的 `s1` 带 `"color": "80c0ff40"` + `"dark": "204060"`。
    * **dark 的 alpha 必须断言 == 1**（只要 `has_dark` 成立）—— 它是参考实现**写死**的
      （`SkeletonBinary.cpp:188` / `SkeletonJson.cpp:230`），不是插值出来的。
      且 `has_dark` 要看 **flag**，不能看"dark 是否全 0"（3.7 的判据是 `int32 != -1`，
      纯黑照样是"有暗色"）。
    * **`setup_attachment_name_offset` 必须解析回真名字**，且解析出来的是**第 4 组**
      字符串那条（手算一遍期望偏移）—— 指到槽位名那组不报错，只是 attachment 时间轴
      复位到一个错名字的附件上。
15. **解算必须拿"读回来的容器"再算一遍**（`doc/GPU_SPINE_V7_ANIMATION.md` §7.0.4）：
    同一个动画在 6 个时间点上，`asset` 与 `back`（C++ 读回的）必须给出**完全相同**的
    (color, dark, attachment, draw_order)。写端对了、读端错了，往返的逐字节断言
    **全**看不出来 —— 只有解算能抓到。
    * 解算侧还有一条独立的坑：drawOrder 的重建**不是**"把槽位挪到 index+offset"
      （`SkeletonBinary.cpp:1040-1072`），且槽位下标必须**严格升序**
      （否则参考实现 `while (originalIndex != slotIndex)` 永不终止）⇒ 夹具要 **2 帧**，
      第二帧才分得开（1 帧时两种实现都给出 setup 序）。
16. **约束 setup 的语义断言**（§3.7，v11）—— 全部是"错了不报错"的，逐条对应一类静默错：
    * **夹具必须含非默认值的三类约束**：IK 带 `bend_dir = -1`（`bendPositive:false`）
      + `mix ≠ 1` + compress/stretch/uniform 三个 flag 至少亮一个；transform 的
      6 个 offset + 4 个 mix 全非 0/1、flags 亮 `relative|local`；
      path 的三种 mode 都**不是**默认值。
      全是默认值的话，"写端根本没解析约束"也会被判通过（读回来还是默认值）。
    * **`order` 必须是 0..N-1 的排列**（N = 三类约束总数），导入期与读端**都要**报错：
      参考实现按 `i` 找 `order == i` 的那条，找不到就静默跳过；负数还会以
      `int == size_t` 比较变成天文数字，同样静默（`Skeleton.cpp:147-175`）。
    * ⚠️ **json 的布尔必须被当成 0/1 读**：Spine 的 json 解析器把 `true/false` 写进
      `_valueInt`（`Json.cpp:165,173`），`getInt()` 读的正是它（`Json.cpp:89-91`）。
      只认数字的话 `bendPositive:false` ⇒ 回落默认 **1** ⇒ **方向反了且不报错**；
      `constantSpeed:false` 同理会变成 1。
      ⇒ **只有 `.skel` ↔ json 逐字段对拍能抓到**（实测就是靠它抓出来的：
      json 侧曾得到 `bend=1,flags=0`，skel 侧是 `bend=-1,flags=5`）。
    * **path 的 `target_slot` 越界判据是 `slot_count`，IK / transform 的 `target`
      越界判据是 `bone_count`** —— 字段名都叫 target、偏移都是 @8，但取值域不同。
      夹具里两边都要有**非 0** 的下标（全是 0 时"写错成恒 0"测不出来），
      且各自要有一条**越界**的负向用例。
    * **`path_index` 的判据必须是名字匹配，夹具要放两条 path**：同一槽位挂
      `p1`（= setup 附件）与 `p2`，setup 指向 `p1` ⇒ `path_index == 0`；
      改成指向 `p2` ⇒ `path_index == 1`。
      ⚠️ **只有第二条分得开"取第一条"与"按名字取"** —— 单条时两者恒等。
      第三条：setup 指向一个**不是** path 的附件 ⇒ `kNoPathAttachment`（且它
      **不是 0**，0 是合法下标）。
    * **chunk 19 只在有约束时落盘**，且这条"可证恒等"要钉住：无约束资产读回来
      重建出 `0..bone_count-1`、reset 为空 ⇒ **读→写仍逐字节相同**。
      反向：有 chunk 18 却**没有** chunk 19 ⇒ **必须报错**（负向用例）。
    * **`_updateCacheReset` 也要断言**（`Skeleton.cpp:200-211`）：少复位会让上一帧
      的应用值残留，表现是**动作拖影** —— 夹具里至少一根骨骼被约束改动过 ⇒ reset 非空。

---

## 13. 决策状态

### 13.1 已拍板

| # | 问题 | 结论 | 落地处 |
|---|---|---|---|
| 1 | Spine 版本 | **3.8**，升 4.x 单独排期（理由：可归因，不是"3.8 更好"） | §1.1 |
| 3 | Clip 策略 | **从"策略"降级为"约束"**：3.8 只有 CPU 实现 ⇒ 带裁剪的槽位走 CPU 动态 VB，同帧可混跑 | §1.1、§7 |
| 9 | 裁剪的回退粒度 | **逐槽位**（不是整个资产）：作用域是 `(clipSlot, endSlot]`，逐皮肤模拟参考实现的状态机得出；**作用域不落盘**，只表达成作用域内绘制项的 `kind = kFallback`。**裁剪多边形要落盘**（chunk 9，12 B/条，顶点复用主顶点缓冲），绘制项加 `clip_index`（24 B → 28 B）。`kHasDeform` 只由 deform 点亮，不由裁剪点亮 | §7.2 |
| 6 | root 变换存在哪？ | **方案 A：实例项加 `root_index`（16 B → 20 B）**，root 存每帧 root 表，24 B/项。选它而不是 B/C 是因为它不改 `bone_base` 的语义，代价只是每绘制项 4 B | §5.2、§5.3 |
| 2 | `MAX_INFLUENCES` 怎么定？ | **改为逐顶点变长（2026-10-22 重拍）**：定长 4 槽在 OSGame 782 个资产实测下不成立（每顶点影响数最大 15、均值 2.20，>4 的占 4.1% ⇒ 卡死 297 个资产）；定长 16 槽要 176 B/顶点，与降内存的目标相反。⇒ weighted 顶点 48 B → **8 B 引用**，蒙皮数据进 chunk 21 / 22。`MAX_INFLUENCES` 保留为 **255**，只当"数据坏了"的硬上限，遇 `n > 255` 仍然报错并报出附件名 | §3.1 |
| 4 | 骨骼通道 structured 还是 UBO？ | **默认 structured / SSBO，24 B 紧凑**；仅当目标后端只有 UBO 时才用 32 B std140 变体（记录在 `bone_stride`） | §5.1 |
| 7 | 容器加 skin 维度怎么加？ | **附件表多一列 `skin_id`**（4 B → 8 B），而不是另建一张 skin 表。选它是因为换装查询就是"按 (skin, slot) 查附件 id"，一列就能表达；另建表会多一次间接跳转却不省什么 | §9.1 |
| 8 | linked mesh 的几何怎么省？ | **UV 拆成独立通道 + 顶点逐字节去重**，两条一起做。只拆 UV 不通（顶点仍按附件各存一份），只去重不拆 UV 也不通（UV 不同 ⇒ 字节不同 ⇒ 去重命中率为 0）。去重**只认逐字节完全相同**，不做近似 —— 宁可少共享，不可错共享 | §3.2、§14.2 |
| 11 | deform 怎么回退？ | **不做稠密展开，也不做 OSGame 式全量 CPU 动态 VB** —— deform 是蒙皮之前的局部空间加数（`VertexAttachment.cpp:122-123`，rigid 则是 `:72-74` 整体替换）⇒ 走 **deform 增量**：共享 bind pose VB 不动、蒙皮仍在 GPU，每实例每帧只上传当前帧 deform 数组（实测均值 122 B/条，对比全量动态 VB 的 24.4 KB/帧）。OSGame 引擎（`SpineMesh.h:266`）那种世界空间全量动态 VB 被否决：它每实例一份 VB，**正好废掉 §0 的 VB 共享** | `doc/GPU_SPINE_V7_ANIMATION.md` §5 |
| 10 | 动画怎么落？ | **A2（2026-10-22 拍板）**：骨骼树 + 动画烘焙进容器 → **v7**，解算器自己写，仍在 CPU（与 §0 一致）。否决 A1（运行时持有 spine 源数据 / 引入 spine-cpp），因为它与 §11.2「容器由 C++ 唯一产出」相冲，且 Unity 侧要跟着搬一整份运行时。范围与边界见 `doc/GPU_SPINE_V7_ANIMATION.md` | §11 |
| 12 | 影响流怎么去重？ | **先对影响流（12 B/条）按"逐字节相同"去重，再用去重后的块起点改写顶点块里的本地下标，最后才对顶点块去重**。顺序颠倒不报错：先改写顶点块的话两个相同附件的本地下标都是 0 ⇒ 顶点去重看着命中了，但它们指的其实是各自那段影响流 ⇒ 其中一个取到另一份的 bindPos | §3.1 |
| 13 | 执行顺序（chunk 19）落"结果"还是落"规则"？ | **落 `Skeleton::updateCache()` 算出来的结果**（`_updateCache` + `_updateCacheReset` 原样落盘），而不是让运行时按 `sortBone` 递归 + 三类约束共用的 `order` 重排一遍。重排再写一遍就是给自己多一次写反的机会，而写反的表现是"姿势略偏"，查不出来。落结果是唯一能对齐的 | §3.7.2 |
| 14 | 3.7 与 3.8 的约束数学不一致怎么办？ | **按 3.8 实现**。逐文件 diff 出 2 处真差异（都是 3.8 修的 3.7 bug）：`Bone::updateWorldTransform()` 根骨骼分支把 `sx`/`sy` 用反；`IkConstraint::apply()` 单骨骼分支 `&&` 绑定优先级 ⇒ `b≈0` 且 compress 时 NaN。3.7 的错解算出来不报错，只是姿势错 | `doc/GPU_SPINE_V7_ANIMATION.md` §4.1 |
| 15 | 同一槽位挂两条 path 附件算不算错？ | **不算，两条都烘焙。** 实测 6 / 781 资产这样（如 `K_Lubandashi_Spine` 的槽位 `EFFTAIYILIZI`）。参考实现 `sortPathConstraint` 会遍历**全部**；我最初加的"重复 ⇒ 报错"一次卡掉 12 个真资产（769/41 对不上 781/29 基线），已撤回 | §3.7.3 |
| 16 | 宿主渲染路径（2026-10-26 拍板） | **列为硬前提：宿主必须允许 per-instance 骨骼参数（§5 的 `bone_base` / `root_index` 可逐实例寻址），Spine 不能走「交付顶点参与合并」的路径。** 共享与实例化**正交**：共享省内存+每帧带宽（收益主体），实例化只省 draw call（可选，**不是共享的前提** —— 不实例化时 VB 仍只有一份、每帧仍只传骨骼表）。宿主一旦走 Canvas 顶点合并（`OnPopulateMesh` / `CanvasRenderer::SetMesh`），顶点须回 CPU 蒙皮 ⇒ 共享归零、上传 2.62x 与 geo 2.34x 归零，而 anim 慢 1.43x 仍要付 ⇒ **净亏**（只有 §3.2 的资产内部去重存活）；不带 deform 的资产可借"几何不脏就不重建"吃到例外，但 deform 覆盖 93% 资产。**OSGame 现状满足**：`UnitySkeletonGraphic` 走 `m_MeshRenderer->SetSharedMesh(m_Mesh)`（`UnitySkeletonGraphic.cpp:377-390`），`m_CanvasRenderer` 只用于 `EnableRectClipping`（`:432-451`）、从不 `SetMesh` ⇒ 挂 UI 层级但不参与 Canvas 合批 ⇒ **"实例化会打断合批"不成立（没有合批可打断）**。实例化的真实代价是与其它 UI 元素的**交错遮挡排序**，按实际 UI 结构判断 | §0.1、§0.2 |
| 17 | 骨骼表 / 静态几何走什么通道？（2026-10-26 拍板） | **默认 SSBO（structured buffer / instance data）；"不用纹理"的理由只保留"显存打包浪费"一条。** ⚠️ 同时**修正一条旧表述**：原文"纹理采样会引入与 CPU 不一致的舍入"只对**带滤波的归一化采样**成立，`texelFetch`（整数 texel 坐标、不滤波、RGBA32F / R32UI 原值返回）**与 SSBO 逐位一致** ⇒ 准确说法是"否决纹理 + 归一化坐标 + 滤波这条路径，不是否决纹理本身"。**触发备选的条件**：目标后端无 SSBO（GLES3 / ES 3.0 没有；Unity 的 `StructuredBuffer` / `ByteAddressBuffer` 要 SM5.0+，GLES3 直接编译失败而非降级）⇒ 走 §5.4：骨骼表 UBO（41 骨 × 24 B ≈ 1 KB，远低于 ES3.0 保证的 16 KB）+ 静态几何 `texelFetch` 纹理（UBO 装不下：中位 33.7 KB / p90 97.7 KB；顶点属性定长展开不可行：影响数最大 15）⇒ **付掉跨资产合批**（对 OSGame 不亏，§0.2：本来就没合批）。**未拍板的部分**：是否上 array texture 救回合批（256 层上限 / 分桶 / 槽位分配），以及 §5.4 整条路线本身 —— 都要先过三项实测（RGBA32F 可采样 / `texelFetch` 逐位一致 / array texture 层数上限）—— 完整清单见 `doc/GPU_SPINE_NO_SSBO_VERIFY.md` | §5.1、§5.4 |
| 18 | OSGame 特性集 / 是否申请例外？（2026-10-26 拍板） | **规则：GPU-Spine 在 OSGame 上只能用现网 shader 已用到的特性** —— 实测口径 `I:\OSGame\...\Assets` 下 `*.shader`(1784) + `*.cginc/*.hlsl/*.glslinc`(675) = **2459 文件**（§5.5）。**路线选择 = B「申请例外」**：申请 `#pragma target 3.5`（现网 256 个 shader 已在用，Spine 自己未声明才需申请；要它是因为 `SV_VertexID` / `SV_InstanceID` 需要该档）+ `texelFetch`（**ES3.0 core 指令、非扩展**，线性滤波才需要 `OES_texture_float_linear`）⇒ 批则 §5.4 落地。**`precise` 放弃**（GLSL ES 3.00 没有，申请不到）⇒ FMA 收缩无法禁止 ⇒ bit-exact 判据在 GLES3 后端不成立，降级为**误差阈值**（§17 底噪取法）；Vulkan 后端能否保留 `precise` 待实测，保留则只在 Vulkan 上维持 bit-exact。**驳回降级顺序**：①`texelFetch` 被否 ⇒ 静态几何走 `tex2Dlod` + Point 采样 + W/H 取 2 的幂（§5.5 合规，199 文件在用，但逐位性更脆弱，需单独对拍 = 验证清单 G2 第 11 项）；②再否或误差超底噪 ⇒ **回退路线 A**（只做 ZEngine / PC / 编辑器，OSGame 维持 CPU 蒙皮） | §5.4、§5.5、§5.6 |
| 19 | ShaderLab 能否同时兼容 ZEngine 与 Unity？（2026-10-26 拍板） | **结论：语法层"是 Unity 格式的子集"，工程上"还不能"，且对 Spine 而言两端都写不完整 ⇒ 不做全量 ShaderLab 化，改为"先抽公式层"。** 三条依据：①**两端都声明不了 Spine 的 9 个全局 buffer** —— Unity ShaderLab 的属性类型没有 Buffer，**ZEngine 的 `PropertyType` 也没有**（`ShaderLabAsset.h:15-26` 只有 Float / Int / Range / Color / Vector / 2D / Cube / 3D / FloatArray）⇒ b0-b8 只能靠**外部绑定契约**，`.shader` 文件本身不承载绑定信息；②**通道模型不可统一** —— ZEngine（Vulkan / DX12）走 SSBO，Unity 侧 Android = **GLES3**（§5.5）只能走纹理 + UBO ⇒ 同一份 HLSL 不能同时成立，必须 `#ifdef` 分叉或两个 SubShader；③⚠️ **ZEngine 的 ShaderLab 自身未完成** —— `ShaderLabParser.cpp:62-63` 明写 `ParsePragma()` **只有声明、没有实现、也从来没有调用者** ⇒ `#pragma vertex / fragment` 目前不解析，入口永远取默认 `"vert" / "frag"`；且 Spine 的 Vulkan / Metal 路径要**构建期**烘焙成 SPIR-V 头文件（`SpineGpuRenderer.cpp:32-37`），ShaderLab 侧尚无此步骤。**附一条好消息**：`ShaderLabHlslExtract` 会剥掉所有 `#pragma` 行（`.h:29-32`）⇒ Unity 需要的 `#pragma target 3.5` 在 ZEngine 侧被忽略而非报错，两者不冲突。**能统一的**：文件组织、渲染状态（4 种混合可从 `pipelines[layer][4]` 收敛进 `.shader`）、§6 公式文本；**不能统一的**：绑定、通道。**为什么不做**：真正的"两端一致"保证是 §6 公式 + `SpineGpuEmulateVertex` + 逐位对拍，**不是同一个文件**；且 OSGame 侧还在等 §5.6 例外结果 —— 若回退路线 A（只做 ZEngine），Unity 兼容这块白做。**改用便宜一个数量级的做法**：抽出**只含 §6 / §8 公式的公共层**，两端外壳各自写（ZEngine 继续 GLSL / HLSL 或以后再 ShaderLab 化，Unity 写自己的 `.shader`）。**已落地（2026-10-26）**：`engine/shader/include/spine_skinning.h`（跨 GLSL / HLSL，沿用 `gbuffer.h` 的 `ZENGINE_HLSL` 方言约定），导出 `SpineAffineApply` / `SpineBoneApply` / `SpineRootApply` / `SpineInfluenceBoneIndex` / `SpineInfluenceWeight` / `SpineApplyDeform` / `SpineShade` + `SPINE_FLAG_*` 常量；`glsl/spine_gpu.{vert,frag}`（`GL_GOOGLE_include_directive`）与 `hlsl/rp2/spine_gpu.*.hlsl`（DXC，`include_paths` 加 `shader/include`，`SpineGpuRenderer.cpp` / `SpineGpuDx12RasterTest.cpp` 两处）都改为 `#include` 它。**验证**：`SpineGpuDx12RasterTest` **6/6 PASS**（RTX 5080，用引擎真正加载的那两份 hlsl）；`SpineSkinningParityTest` **11/11 PASS**，max 7.629e-06 / 0.68 ulp（与改动前记录的基线一致）；GLSL 两侧 `glslangValidator -V100` 与 HLSL 两侧 `dxc` 均通过。⚠️ 已知差异：glslang 不内联 ⇒ SPIR-V 里出现真正的 `OpFunctionCall`（驱动会内联），`NoContraction`（`precise`）装饰在两版里都保留，除法 `/255.0` 未被改成乘倒数 | §5.5、§6、§8 |

### 13.2 仍待拍板

**路线层面无悬而未决项**（#2 在 2026-10-22 因实测改判为"逐顶点变长"）。
#18 的两个例外项**待批准**，其路线与驳回降级顺序已拍板在 §5.6；
#17 的 array texture 取舍与 §5.4 整条路线仍待 G0 / G0' 实测（清单见
`doc/GPU_SPINE_NO_SSBO_VERIFY.md`）；#19 的公式层**已落地**（`engine/shader/include/spine_skinning.h`）；剩下的 Unity 侧外壳
（走纹理 + UBO 通道）**依赖 §5.6 例外结果**，属于待排期行动项而非决策悬置。
后续若再开新议题写在这里。

（v7 设计内部的开放项，记在 `doc/GPU_SPINE_V7_ANIMATION.md` §7：
**槽位 setup 已落地**（v10，chunk 17，§3.6）；
**约束 setup 已落地**（v11，chunk 18 / 19 / 20，§3.7）—— 落的是 setup 与执行顺序，
**解算本身（S4b）还没写**：骨骼时间轴（rotate/translate/scale/shear）+ 三类约束求解 +
按 chunk 19 遍历，按 3.8 数学实现；
chunk 15/16 的曲线与变长载荷编码**已定**——紧凑曲线（u8/段 + 16 B bezier）+ 稀疏 deform，
见 §3.5。）

---

## 14. 已知限制

### 14.1 （已修，2026-10-21）~~容器里没有 skin 维度~~

v2 已补上：附件表多一列 `skin_id`（4 B → 8 B），绘制项尾部填充改成 `skin_id`
（v2 仍是 20 B，v3 起因 `uv_offset` 变 24 B），header 的 `reserved` 改成 `skin_count`，
字符串表插入皮肤名。
附件 = (皮肤, 槽位, 附件名) 三元组，**不再跨皮肤合并**。见 §9.1。

### 14.2 （已修，2026-10-21）~~linked mesh 的几何是复制的，不是共享的~~

v3 已解决，且**当初列的两条路都做了**（缺一不可，见 §13.1 第 8 项）：

1. **UV 拆成独立通道**：顶点只剩"几何 + 蒙皮"（rigid 16 → **8 B**，weighted 56 → 48 B →
   **v7 再降到 8 B 引用**，蒙皮数据搬去 chunk 21 / 22），
   UV 进 chunk 4 的 `uv_buffer`（`float2` × `uv_count`），由 `DrawItem::uv_offset` 索引。
2. **顶点逐字节去重**：`ExactBlobPool` 按"整段顶点字节完全相同"合并，FNV-1a 只用来缩小比较范围，
   最终判定永远是 `memcmp`。

现在 linked mesh 与父附件**共用同一段顶点**，各自只多一份 UV。测试里量到的实际收缩：
单皮肤 linked mesh 8 → **4** 个 weighted 顶点；跨皮肤 linked mesh 12 → **8**。

剩下的代价只有一条：shader 里多一次间接读取（先 `indexBuffer[...]` 拿顶点下标，
再 `uv_buffer[drawItem.uv_offset + vertexIndex]`）。这条**不能省** —— 它就是"能共用"的前提。

（历史：#1 版本、#2 影响数、#3 Clip、#4 骨骼通道、#5 root 是否 per-instance、#6 root 存放方式。）

### 14.3 （已修，2026-10-21）~~裁剪多边形本身不烘焙~~

v6 已补上：chunk 9 写 `ClipPolygon[]`（12 B/条），绘制项加 `clip_index`（24 B → 28 B）。
多边形顶点**复用主顶点缓冲**（chunk 3）—— `ClippingAttachment : VertexAttachment`，
编码方式与 mesh 顶点完全相同、走同一个 `ExactBlobPool` 去重，所以 header 不用新增字段。

（历史：v5 时这里写的是"多边形不烘焙，运行时从 spine 源数据取"。这不成立 ——
`kFallback` 的语义只说"别用静态 VB"，而"只靠容器渲染、不再持有 spine 源数据"是
整个容器格式的目标之一，裁剪多边形是**静态资产几何**（和 deform 那种逐帧数据不同），
必须能落盘。嵌套裁剪曾经也列在这里 —— 规则一直是明确的，但夹具里没有"两个裁剪附件叠
在一起"的骨架，因为第二条裁剪要**占掉**一个槽位、那个槽位就没有几何了。现已补上
5 槽位夹具，判据是**最后那个槽位**的 `kind`：见 §12 第 10 项。）

### 14.4 （已修，2026-10-21）~~容器在 Unity(C#) 侧的读端~~

已补上：`Function/Render/Spine/CSharp/`（`SpineGpuFormat.cs` + `SpineGpuContainerIo.cs`，
netstandard2.1，可整目录拷进 Unity Assets），并接进 ctest：
`SpineGpuImportTest --dump` 落盘 → C# 读回 → C# 再写 → 逐字节比对（§11.1）。

**C# 导入器不做**（拍板，2026-10-22）：从 spine 源数据在 Unity 侧产出同一个容器
这条路径**放弃**了。理由就是上面那条 —— "两端各写一遍导入器"是漂移的温床，而
Unity 侧真正要的只是**能读**。产出唯一地走 C++：

```
ZEditor -batchmode -suite=SpineCook <hero.json | hero.skel | 目录> [-o <输出>] [--recursive]
```

`SpineCook` 是 ZEditor 的一个**工具套件**（`Source/Editor/ToolSuite/SpineCook/`），
`-batchmode` 不弹窗、不启动引擎、不初始化 RHI，直接跑烘焙（见 §11.2）。目录输入
保持相对结构铺开，同名骨架不会互相覆盖；有失败就退出码非 0。

### 14.5 v8/v9/v10/v11 已补上骨骼树、动画、槽位 setup 与约束 setup（**边界**按 OSGame 实测反推，这部分永久有效）

> **进度**：v8 = 骨骼树（chunk 10，**已落地**，§3.4）；v9 = 动画（chunk 11-16，
> **已落地**，§3.5）；v10 = 槽位 setup（chunk 17，**已落地**，§3.6）；
> v11 = 约束 setup + 执行顺序 + PathAttachment（chunk 18 / 19 / 20，**已落地**，§3.7）。
> 剩下的**只有约束解算**（S4b）与 GPU 蒙皮上线路 —— 下面"边界怎么定的"这部分不随进度失效。
>
> **S4b 已落地（2026-10-22）**：解算器 `SpineGpuPoseSolver` + **参考实现对拍台**
> （`Test/SpineGpuRefParityTest`，把 spine-cpp 3.8 编进来逐骨骼比）。
> 实测 **781 / 781** 个资产逐骨骼全对（9 660 个采样、2 410 290 次分量比较，最大相对误差 1.5e-5）。
> 对拍台抓到的四个 bug（全是"不报错、只是姿势偏"）：**(a)** chunk 20 的偏移没用；
> **(b)** path 的 deform 被跳过（62 个，⇒ v12）；**(c)** `updateAppliedTransform` 写成了
> "applied = 局部值"（13 个）；**(d)** 曲线进度没逐字照抄参考实现的浮点写法（1 个）。
> 详见 `doc/GPU_SPINE_V7_ANIMATION.md` §7.0.6。

v6 及以前只烘焙**绑定姿态**：`SpineGpuAsset` 有 `bone_names` 但 `SpineSourceBone::parent`
在 Bake 时被丢掉（`SpineGpuImporter.h:78`），也**没有任何动画 chunk**（chunk 只到 9）。
⇒ 那时一个 `.zspg` **只能画绑定姿态**，动不起来；骨骼矩阵得由外部（spine 运行时）喂。

同时 `kHasDrawOrderTimeline` / `kHasClipping` 这两个 flag 的语义是"走 CPU 路径"，
而 **ZEngine 目前根本没有 CPU 路径** —— 它们指向一条不存在的路。

v7 的处理（见 `doc/GPU_SPINE_V7_ANIMATION.md`）。**边界不是拍脑袋定的**，是扫了
`G:/OSGame/Client_proj/Project/Assets` 下 **782 个 `.skel.bytes`**（全部 3.7.83，
782/782 解析到精确 EOF、0 失败）反推出来的：

* 必须烘焙 **11 类**时间轴：rotate / translate / scale / shear / attachment / color /
  twoColor / deform / drawOrder / pathConstraintPosition / pathConstraintMix；
  event 与 IK / transform 约束**时间轴**实测 0 条，不做。
* ⚠️ **但约束本身必须解算**：IK 22.5% / transform **48.3%** / path 11.1% 的资产带约束，
  而约束在 `Skeleton::updateWorldTransform` 里**每帧都跑**，与有没有时间轴无关。
  只做骨骼时间轴会让近一半资产姿势静默错。⇒ 约束 setup + 执行顺序也要进容器
  （**v11 已落地**：chunk 18 = 三类约束 setup + 共用骨骼池、chunk 19 = `updateCache()`
  的执行顺序与复位表、chunk 20 = PathAttachment 几何，§3.7）。
  实测带约束 **481 / 781（61.6%）**；三块合计 **0.33 MB**（容器的 0.16%）——
  落盘的代价可以忽略，不落的代价是姿势静默不动。
  ⚠️ 落盘 ≠ 解算：**S4b 还没做**（骨骼时间轴 + 三类约束求解 + 按 chunk 19 遍历）。
* **3.7 vs 3.8 字节布局完全一致**（逐段比对），只有槽位暗色判据不同
  ⇒ 导入器放宽版本 + 一个分支即可，**不需要第二套解析器**。
  现在实测 **0/782 能导入**（版本检查硬拒 3.8 以外的）。
* **deform 是 93% 资产都有**的大头（100 306 条 / 1 733 134 帧 / 2 831 万 float），
  且帧是**稀疏**的 ⇒ 不能展开成稠密数组搬 GPU（GB 级）。v7 保持稀疏烘焙 +
  既有 `kFallback` 语义走 CPU 动态 VB，**只回退被命中的绘制项**。
* 曲线段实测 257 万段（linear 58% / stepped 5% / bezier 37%）⇒ 编码从
  定长 20 B/段改成**紧凑**（u8 类型 + 每条 bezier 16 B），49 MB → 16.8 MB。

---

## 15. 渲染接入（编辑器：Vulkan / DX12 / Metal）

代码：`Function/Render/Spine/SpineGpuRenderer.{h,cpp}` + `engine/shader/glsl/spine_gpu.{vert,frag}`
（构建期编译成 SPIR-V 头文件，不走运行时 glslang；Metal 由 MetalRHI 经 SPIRV-Cross 转 MSL）+
`engine/shader/hlsl/rp2/spine_gpu.{vert,frag}.hlsl`（DX12，运行时 DXC 编，与 RP2 其它 pass 同一做法）。
三个后端接入点与差异见本节末"DX12 / Metal"。入口是控制台命令：

```
spine.load <骨架 .skel.bytes|.skel|.json> [动画下标]   # 图集取同名 .atlas.txt / .atlas；= clear + add
spine.add <骨架> [动画下标] [个数]   # 多实例：加到最上面并选中最后一个；同一路径共用一份资产
spine.remove [id=选中] / spine.select <id> / spine.place <x> <y> [scale] | spine.place auto
spine.clear                          # 删全部实例
spine.status                         # 每个实例一段：id / 文件 / 项数 / 轨道
# 多轨（S5，语义 = spine::AnimationState，见 GPU_SPINE_V7_ANIMATION.md §7.0.7），作用在**选中**实例上（mix 是全局）：
spine.play <动画> [track] [loop] / spine.queue <动画> [track] [delay] [loop] / spine.empty [track] [mix]
spine.mix <秒> / spine.speed <倍率> / spine.alpha <track> <alpha>
spine.tintblack <0|1>   # 双色（= SpineSettings::tintBlack），默认 0，跨资产保留
spine.skin <名字|下标|none> [setup=1]   # 选中实例换肤（= Skeleton::setSkin，setup=1 时再 setSlotsToSetupPose）
# 启动即加载：ZEditor ... --rhi vulkan --cvar "spine.load G:/.../K_X_Spine.skel.bytes 1"
```

**每帧 CPU 只做两件事**：解算（S5 起是 `SpineGpuAnimationState::Update(dt)` → `Apply(state)` →
`SpineGpuUpdateWorld`，骨骼世界变换 + 槽位状态）与"哪些绘制项可见"
（按 `draw_order`，取槽位当前附件——按"先当前皮肤、再 default 皮肤"解析出的那一项）。**顶点一个都不在 CPU 上算**：

| set 0 binding | 内容 | 更新频率 |
|---|---|---|
| 0 | chunk 3 顶点字节（u32 视图） | 每资产一次 |
| 1 / 2 | chunk 21 / 22 影响流 | 每资产一次 |
| 3 | chunk 4 UV | 每资产一次 |
| 4 | 骨骼表（§5.1，6 float/骨） | 每帧 |
| 5 | 可见绘制项（32 B：格式 / uv_offset / 槽位骨骼 / 骨骼基址 / 实例 / 色 / flags / weighted 段起点） | 每帧 |
| 6 | 实例（clip_from_world + 根变换，96 B） | 每帧 |

set 1 = `UIGpuResources` 的贴图布局（图集页）。没有顶点输入流：顶点着色器用
`gl_VertexIndex`（= 索引缓冲给的段内下标）直接取 SSBO，`gl_InstanceIndex`（= `firstInstance`）
选中绘制项 —— 一个绘制项一次 `vkCmdDrawIndexed`。混合按 §8 四条管线（输出预乘 alpha；
OSGame 图集是直 alpha ⇒ 片元里先预乘，additive 写 alpha = 0）。

**画在哪**：`MainCameraPass::DrawRp2` 的 UI subpass、`m_UiPass->Draw()` **之前**（编辑器面板
盖在它上面，与 UI 层同一合成公式）；视口取 Scene，Scene 页签不在布局里（0×0）时取 Game。
setup 姿势的包围盒自动居中、占视口 80%。

**验收（数值，不看图）**：`VK_LAYER_LUNARG_screenshot` 抓第 150 / 160 / 250 帧，与不加载时逐像素比：
`K_Bailishouyue_Spine` 动画 #1（5.33 s）≈ 43–46 万像素变化、包围盒落在预测框内、框外 0 变化；
同一次运行第 150 / 250 帧之间 ≈ 18 万像素变化（在动）。动画 #0 `idle_01` 时长 **0**（单帧
静态姿势）—— 对拍台已新增"时长与参考逐个相等"（1932 / 1932），这是参考实现的真值不是 bug。

**mesh deform（S6，v14）已接**：见 §7.1。set 0 多一个 b7（本帧 deform 数组），绘制项 48 B
（+ `deform_base`，flags bit2 = 有 deform）。实测 `K_Bailishouyue` 动画 #1 首帧 95 个可见项里
38 个带 deform（860 float，容量上界 1010）。

**验收（对拍台，全量 781 资产 × 每动画 5 个时间点）**：
* 槽位当前附件（= 画哪一项）与参考 `Slot::getAttachment()` 一致：779 515 次；
* deform 数组与参考 `Slot::getAttachmentVertices()` 逐 float：200 536 个非空采样、
  1 131 万个 float，**最大相对误差 0**（逐位相同）；
* 影响流"附件内连续"：592 540 个 weighted 顶点；
* 端到端：照 shader 同一套寻址 / 公式在 CPU 上蒙皮，与参考 `computeWorldVertices()` 比世界坐标 ——
  rigid（无 / 有 deform）最大误差 1.2e-4 / 1.2e-4；weighted 5.21 / 5.14 骨架单位
  （**v16 及以前的数字**，误差来自 u8 权重量化，与 deform 无关）。
  **v17（unorm16 权重）后：weighted 0.071 / 0.071**，rigid 不变 → 门槛收紧到 0.1 并接进
  verdict（u8 时代的 5.2 会被挡住）。把 deform 偏移错一个顶点，两格会变成 9724 / 402。

**仍然不做**（不是忘了）：WebGL2（没有 SSBO）。（tint black、DX12 / Metal、多实例、换肤已接，见下面。）

#### v15：运行时裁剪（2026-10-23）

3.8 的裁剪只有 CPU 实现（`clipTriangles` 产生新三角形，VS 做不了）。每帧：

1. `SpineGpuClipFrame()` 按**当前**绘制顺序跑 `MeshGeneration/SpineMesh.h:107-259` 的状态机：
   NULL 附件 / 不可画附件 ⇒ `clipEnd(slot)`；裁剪附件 ⇒ `clipStart` 后**直接 continue**（不收尾）；
   可画项 ⇒ 在作用域内就裁，然后 `clipEnd(slot)`；末尾无条件 `clipEnd()`。嵌套被忽略。
2. 作用域内的项在 CPU 上蒙皮（与 `spine_gpu.vert` 同公式、同 unorm16 权重，v17），裁剪多边形同样蒙皮
   （**含裁剪附件自己的 deform** —— v15 起进容器，target 次高位 0x4000），喂给逐行转录的
   `SpineGpuSkeletonClipping` / `SpineGpuTriangulator`（SkeletonClipping.cpp / Triangulator.cpp）。
3. 结果上传 b8（`vec4 x,y,u,v`，骨架空间）+ 每帧动态 u16 索引缓冲；绘制项 flags bit3，`deform_base`
   改作 b8 起点。两块缓冲按需倍增扩容（旧的走延迟销毁，只重写本帧槽位的 set0）。作用域外照旧 GPU 蒙皮。

region 喂给 `clipTriangles` 的顺序必须照参考：`computeWorldVertices` 出 **BR, BL, UL, UR** + 三角形
`{0,1,2, 2,3,0}`；容器存的是 BL, UL, UR, BR ⇒ 绘制项 @23 的 pad 改成 `item_flags`，bit0 = region，
CPU 裁剪时按 {3,0,1,2} 重排（不裁时两条对角线画出来一样，裁了拓扑就不一样）。

对拍（`SpineGpuRefParityTest` 的 S5 段，781 资产 × 2 段随机脚本 × 150 帧）：

| 项 | 结果 |
|---|---|
| 可画槽位"在不在作用域、被哪条裁" | 658 816 次，**0 分叉** |
| 同输入 `clipTriangles`（喂参考的世界坐标 / UV / 三角形 / 多边形） | 359 772 次，输出 21 229 594 顶点，**逐位相同** |
| 端到端，全 rigid（191 252 项） | 世界坐标最大误差 **3.66e-4**，UV **3.58e-7**（无权重量化，只剩浮点噪声） |
| 端到端，含 weighted（359 772 项） | 拓扑相同 **359 768（99.9989%）**；世界坐标最大误差 **0.194** 骨架单位 / UV **5.46e-4** |

⚠️ **2026-10-29 重测（v17 unorm16 之后）**：上面后两格原本是 v16 / u8 权重时的
**35.5 骨架单位 / UV 0.151 / 拓扑相同 99.84%**，并挂着一句"v17 应随之下移（未重测）"。
重测结果已就地更新 —— **世界坐标 35.5 → 0.194、UV 0.151 → 5.46e-4、拓扑不同从 0.16% 降到
4 / 359 772**。剩下那 **4 项拓扑不同全部是 weighted、全 rigid 0 项** ⇒ 是 unorm16 权重的残差在
**近乎与裁剪边平行的边**上翻了判定，不是 bug（全 rigid 没有权重量化，它要是也翻就是写错了）。

这一格以前**只统计不判定**（画面偏一点不会红）。现已钉成**验收上界**
（`SpineGpuRefParityTest` 结尾那行"裁剪端到端上界"，进 verdict）：

* 世界 ≤ 1.0 / UV ≤ 5e-3（实测 0.194 / 5.46e-4）—— **u8 权重退化回来必定红**（35.5 / 0.151）；
* 全 rigid 那部分：世界 ≤ 5e-3 / UV ≤ 1e-5，且**拓扑不同必须为 0**；
* 覆盖：必须有 `clip_frames > 0 && clip_e2e_items > 0` —— 否则四条全 0、恒真，等于没测。
  种子由资产路径哈希决定 ⇒ 同一批资产上这些数可复现，上界不会随机翻。

**同时修掉的两个 UV bug**（v15 之前参考侧加载器不配图集，UV 从没和参考比过）：

* region：参考 `_uvs` 按**输出顺序** BR, BL, UL, UR 配对（槽位名 BLX/ULX… 只是下标），v14 按槽位名抄，
  每个角错一位 ⇒ **贴图转 90°**。修后 13 028 次角/UV 配对全对。
* 旋转图集上的 mesh：`textureHeight = regionWidth / (v2 - v)` 写成了 `regionHeight`（注释对、代码反），
  宽高不等的旋转 region UV 拉伸。修前前 120 个资产 40% 的 mesh 采样对不上，修后全量 438 302 次 0 不一致。
#### tint black（双色，2026-10-23）

容器格式**不变**（暗色数据 S3 起就在 chunk 17 + TwoColor 时间轴里，S5 起逐槽位与参考对拍）。只接渲染：

* 参考：网格生成 `SpineMesh.h:202-205` 在 `tintBlack` 打开时给每个顶点 `uv2 = (dark.r, dark.g)`、
  `uv3 = (dark.b, 1)`，取槽位**当前** `getDarkColor()`（不看 `hasDarkColor`）；片元按
  `Spine-SkeletonGraphic-TintBlack.shader`：`out = tex·vc + ((1 - tex.rgb)·(_Black + dark)·tex.a·_Color.a·vc.a, 0)`。
* 我们：绘制项 48 B 里原来的 `reserved[3]` 改成 `float dark_r/g/b`，flags **bit4** = 带暗色项；
  `_Color` / `_Black` 取材质默认 1 / 0。加色槽位（PMA 顶点色 alpha = 0 ⇒ 项恒 0）与暗色全 0 的槽位不置位。
  CPU 侧规则与片元的 CPU 版都在 `SpineGpuTintBlack.h`，`SpineGpuImportTest` 有 7 条手算断言。
* **暗色不截断**：参考走 float UV 通道；TwoColor 在首帧之前的符号 bug 会给出负暗色（S5 已记），照样传。
  OSGame 实测这条路径 0 次（见下）。
* **默认关**，由 `spine.tintblack 1` 打开。原因是 OSGame 线上**没有**逐槽位暗色：
  - 工程里所有 `tintBlack` 序列化值都是 0（prefab / 场景 / asset 共 3012 处）；
  - 这个引擎分支的 `MeshGenerator::FillVertexData()`（`SpineMesh.cpp:131-143`）只上传
    位置 / uv0 / 颜色，**`uv2` / `uv3` 算了但从不上传**；
  - 主力 shader `UI_SpecialCard_Spine` 只用材质常量 `_Black`，不读顶点暗色。

  打开后的画面 = 标准 spine-unity 的 TintBlack 效果（即"如果 OSGame 修好 uv2/uv3 上传"）。

覆盖面（S5 对拍段统计，781 资产 × 2 段随机脚本 × 150 帧）：可画槽位暗色非 0 共 **9 234** 次，负分量 **0** 次；
最多的是 `CardHero/Spine/ZhuBaJie/ZhuBaJie_ys/K_ZhuBaJie_Spine.skel.bytes`（4 777 次，单个槽位）。
编辑器实跑（Vulkan + 验证层）：该资产打开 tint black 后每帧 1 / ~120 项带暗色项，无验证层报错。

#### DX12 / Metal（2026-10-23）

渲染器原本就只用通用 RHI 接口，去掉 Vulkan 门槛后要处理的是各后端的**语义差异**（都是"能跑、不报错、画错"类）：

| 差异 | 处理 |
|---|---|
| **接入点** | Vulkan：MainCameraPass 的 RP2 UI subpass（不变）。DX12：编辑器没有 UI 层可画（RP2 的 UI 层是透明兜底图，ZSlate / ImGui 在 RP2 之后直接画 swapchain）⇒ `MainCameraRp2Pass::SetCombineOverlay`，在 Combine UI 之后、同一个 swapchain pass 里画（手写路径与 render graph 路径都接了）。Metal：`RenderSystem::Tick` 的 swapchain pass 里，scene blit 之后、post-UI 回调之前。三处都是"场景之上、编辑器面板之下"。视口统一走 `DrawInEditorViewport`（Scene 窗口开着画 Scene，否则 Game）。 |
| **绘制项下标** | Vulkan `gl_InstanceIndex` 含 firstInstance；Metal `[[instance_id]]` 含 baseInstance（spirv-cross 实测映射成 `gl_InstanceIndex [[instance_id]]`）。**D3D12 的 SV_InstanceID 不含 StartInstanceLocation** ⇒ DX12 管线多一条 per-instance u32 顶点属性（location 3 ⇒ TEXCOORD0），绑一块 0,1,2,… 的顶点缓冲：per-instance 属性取址会加 StartInstanceLocation。 |
| **结构化 SRV 的 stride** | DX12 把 SSBO 建成结构化 SRV，没给 stride 时按 range 猜（80 B 或 4 B）⇒ float2 / 48 B / 96 B / vec4 全错。`kSet0Strides = {4,8,4,8,4,48,96,4,16}` 写进每个 `RHIDescriptorBufferInfo.stride`。 |
| **描述符生命期** | DX12 的 `UpdateDescriptorSets` 把 SRV 建在**当前帧**的堆分区里，该分区同一帧号下一轮整体回收 ⇒ 只写一次的集合第二轮成了野句柄。DX12 每帧录制前重写本帧槽位的 set0 / set1（`RefreshDescriptors`，同 `MainCameraRp2Pass::DrawRP2` 的做法）。也因此图集页不再借 `UIGpuResources`（它的描述符集只写一次），渲染器自己 `CreateGlobalImage`、自己建 set1 布局、随资产延迟销毁。 |
| **根签名重设** | DX12RHI 每次 `CmdBindDescriptorSets` 都 `SetGraphicsRootSignature` ⇒ set0 / set1 改成**一次**一起绑，不依赖"同一根签名不清绑定"。 |
| **裁剪空间 y** | Vulkan NDC y 向下（不翻视口）⇒ sy 取负；DX12 / Metal NDC y 向上，两边 RHI 都不翻视口 ⇒ sy 取正（`ClipSpaceYSign`）。 |
| **Metal 槽位** | MetalRHI 每个 set 一段 8 个槽（`k_msl_descriptor_set_stride`），set0 的 b8 落到 set1 的第 0 槽 —— 但 b8 是 buffer（`[[buffer(16)]]`）、set1 只有 texture（`[[texture(8)]]`），两种资源各自编号，**不冲突**。以后 set1 若加 buffer 就会撞。 |

验证：

* `SpineGpuDx12RasterTest`（新，ctest）：拿引擎**真正加载的那两份 HLSL** 画进 64×64 离屏目标、回读像素，
  根签名按 DX12RHI 同一映射手搭、SRV 用同一张 stride 表。6 条断言覆盖：y 方向、绘制项下标含 StartInstanceLocation
  （第 0 项故意放右上角且从不以 firstInstance = 0 画）、weighted 权重（换位会让边界列空掉）、tint black 公式、
  b8、tint 字节序。变异：VS 改成 `draws[0]` / y 取反都是 1/6。
* 编辑器实跑 DX12：K_Bianque（裁剪 55 项）、K_ZhuBaJie（tint black）各 60 s，不崩、无 device removed；
  PSO 创建成功（DXC 编 HLSL + 根签名 / 输入布局与 shader 匹配是在建 PSO 时校验的）。
  ⚠️ DX12RHI 只在建设备和 device removed 时抽调试层消息；临时每帧抽取 + 故意传负宽视口也抽不到消息
  ⇒ **这台机器上调试层的逐帧校验不可观测**，"无调试层报错"不能当证据。
* Vulkan 回归：同两份资产实跑，validation 层无报错（图集页改为自建、set 改为一次绑之后）。
* Metal：**没在 Apple 上跑过**（这台机器没有 Apple 工具链），但"SPIR-V → MSL"这一环已经从
  "得有一台 Mac 才发现得了"挪到了 CI：`MetalSpirvShaderSmokeTest` 新增的 D 段在**主机上**跑
  SPIRV-Cross（纯 C++，Windows / Linux 都编得出），按 `MetalRHI::CreateShaderModule` 同一套配置翻译，
  再直接断言**产物 MSL 的文本**（不是"我们推算的槽位 == 我们推算的槽位"）：
    - `spine_gpu_vert`：9 个 SSBO 正好落在 `[[buffer(8..16)]]`，一个不多一个不少（多一个就顶到 17、
      跟顶点 buffer 槽位撞；少一个说明有 binding 没拿到槽位，SPIRV-Cross 会自己从 0 编号）；
      `gl_InstanceIndex → [[instance_id]]`、`gl_VertexIndex → [[vertex_id]]`；`spvFMul` 在 ⇒
      `invariant_float_math` 生效（丢了它 fma 会被拆成 mul+add，跟 Vulkan 的逐位一致性没了且不报错）；
    - `spine_gpu_frag`：`Draws` 在 `[[buffer(13)]]`、图集在 `[[texture(8)]]` / `[[sampler(0)]]`；
    - 所有内置 blob 的 `[[sampler(n)]]` 全部 <= 15（MSL 硬上限）。
  另有 4 条把"测试里的期望值"与"实现里的常量"对齐（读 `MetalRHI.mm` 文本，防止两边一起漂）：
  `k_msl_descriptor_buffer_offset = 8` / `k_msl_descriptor_set_stride = 8` / `k_msl_max_sampler_index = 15` /
  `baseInstance:firstInstance`。
  变异（把 buffer 偏移 8 → 9）：顶点与片元各红一条 ⇒ 不是空断言。实测 **73 checks / 0 failures**
  （Windows，`ctest -R MetalSpirvShaderSmokeTest`）。
  **仍未覆盖**：转出来的 MSL 过不过 Metal 的运行时编译器、以及实际像素 —— 这两步还是要 Mac。
* `[[instance_id]]` 含 baseInstance 已按 Apple 官方文档钉死：`drawIndexedPrimitives(...baseInstance:)`
  写的是"instance_id 从 `baseInstance` 递增到 `baseInstance + instanceCount - 1`"。Spine 是引擎里
  **唯一**用非零 `firstInstance` 的调用方（其余 pass 全传 0，`mesh.vert` 那种
  `mesh_instances[gl_InstanceIndex]` 也是配 instanceCount 用的）⇒ 这条语义此前没有任何代码路径验证过。
* 画面没有人眼看过：屏幕截图（GDI BitBlt）在这台机器上得到全黑（桌面不可见）。

#### 多实例（2026-10-23）

* **资产 / 实例分离**：`Asset` = 容器 + 查找表（`SpineGpuRuntimeData`，只读）+ 静态缓冲 + 图集页 + 每帧缓冲 +
  描述符集，按**路径**缓存、`shared_ptr` 共享；`Instance` = 骨架状态 + `SpineGpuAnimationState` + 摆放 + 本帧
  姿势 / 裁剪结果。最后一个实例删掉时资产延迟销毁。文件改了要 `spine.clear` 再加（缓存不看时间戳）。
* **拼包**（`SpineGpuBatch.{h,cpp}`，纯 CPU，单测 / 对拍直接用）：同一资产的实例首尾相接拼进同一组每帧
  缓冲 —— 着色器早就按 `SpineDraw.bone_base` / `inst_index` 寻址（第一版恒为 0，**从没测过**），这次才真用上：
  | 缓冲 | 实例 k 的偏移落在哪 |
  |---|---|
  | b4 bones | `bone_base` = 前面实例的骨骼数之和 |
  | b5 draws | 实例 k 的绘制项是一段连续区间；firstInstance = **合包里的全局下标**（DX12 的 draw_ids 按需倍增） |
  | b6 insts | `inst_index` = k |
  | b7 deform | `deform_base` 的回绕偏置里已含前面实例的 deform 长度 |
  | b8 cpu_verts / 动态索引 | `deform_base`（bit3）与命令的 `index_first` 已含前面实例的裁剪顶点 / 索引数 |
  每帧容量按需倍增（旧缓冲延迟销毁，只重写本帧槽位那一格）。一个实例解算 / 拼包失败只跳过它自己（合包原样回滚）。
* **录制**：按实例顺序（先加的在下面）；换资产时整套状态重绑。
* **摆放**：自动 = 视口网格（列数 ≈ √(n·宽高比)，行从上往下），每格里 setup 包围盒居中占 80%（1 个实例时与
  多实例之前**逐位相同**）；`spine.place x y [scale]` = 骨架原点在视口中心偏 (x, y) 像素（y 向上）、scale
  像素 / 单位。失败的实例也占格子（别的不跳动）。
* **命令**：实例增删 / 摆放 / 播放进**同一条**有序队列；目标在入队时解析成 id（游戏线程的"选中"）⇒
  `spine.add` 之后紧跟的 `spine.play` 作用在新实例上。加载失败时渲染线程把 id 摘掉、选中退回最后一个。
* **描述符集复用**：RHI 没有 FreeDescriptorSets；资产销毁时它的 set0 / set1 到期后进空闲表，下一个资产先拿
  空闲的（以前每 `spine.load` 一次就从共享池漏一组）。
* **验证**：
  * `SpineGpuRefParityTest` S5：每 4 帧把**上一帧的姿势**当另一个实例排在前面、当前帧排在后面，当前帧每个
    顶点（`SpineGpuEmulateVertex` = spine_gpu.vert 的 CPU 版，越界即报错）与它单独成包时**逐位相同**：
    781 资产 57 794 帧、1.70 亿个顶点（裁剪项 561 万、带 deform 6 350 万），0 分叉。
    变异 5 处（bone_base / inst_index / 裁剪顶点基址 / 动态索引基址 / deform 段相对化）**全部**抓到。
  * `SpineGpuImportTest`（ctest）+15 条：网格 / 自动 / 手动摆放手算，两实例偏置、逐位一致、反例有判别力、
    坏姿势回滚、越界仿真报 false（1393 → 1408）。
  * `SpineGpuDx12RasterTest`：本实例的骨骼从 bone_base = 3、变换从 inst_index = 1 取（前面放别的实例的诱饵）；
    HLSL 去掉 bone_base ⇒ 3/6、取 insts[0] ⇒ 2/6。GLSL 与 HLSL 同公式，GLSL 侧没有 GPU 级测试。
  * 编辑器实跑（Vulkan 开验证层 / DX12）：K_Bianque ×2 + K_ZhuBaJie ×3、删一个、手动摆一个、加一个不存在的
    文件 ⇒ 4 实例 / 2 资产、首帧 224 项、裁剪 110 项（两个 K_Bianque 各 55），无验证层报错、无 device removed；坏文件只报一行错。

#### 换肤（非 default 皮肤，2026-10-23）

容器**不改**（v15）：绘制项 / path 附件 / 裁剪多边形 / 附件记录早就带 skin 列，deform 时间轴的目标是
**按皮肤区分**的附件记录，缺的只是运行时"槽位当前挂的是哪个皮肤里的那一条"。

* **附件身份 = (名字, 来自哪个皮肤)**（`SpineGpuAttachRef::skin`）。参考实现按 `Attachment*` 判同一附件；
  同名附件在不同皮肤里是不同对象 ⇒ 换肤后 deform 要清。以前只按名字判。
* **解析**（`SpineGpuResolveAttachment` = `Skeleton::getAttachment`）：先当前皮肤、再 default 皮肤。查找表
  `SpineGpuRuntimeData::skin_hits[皮肤][(槽位, 名字)] → {绘制项, path, 裁剪}`；setup 附件、attachment 时间轴、
  setToSetupPose 挂附件时都按骨架**当前**皮肤重解析。顺带修了两个潜在错误：path 附件以前按"槽位 + 名字"取
  **任意皮肤**的第一条；二进制里 default 皮肤为空时皮肤 0 是第一个具名皮肤，以前会被当成 default。
* **setSkin**（`SpineGpuSetSkin`，照 OSGame 3.8 分支 `Skeleton::setSkin` / `Skin::attachAll`）：
  旧皮肤非空 ⇒ 只换"当前挂的正是旧皮肤那条"的槽位，新皮肤里没有同名的**不动、不回落 default**；
  旧皮肤为空 ⇒ 按 setup 名在新皮肤里找；setSkin(NULL) ⇒ 附件一个都不动。这个分支**没有** skin bones /
  constraints，`updateCache` 的结果与皮肤无关（path 约束排序本来就遍历全部皮肤）⇒ chunk 19 不重排。
* **deform 上界**改为"每个槽位取所有皮肤里最长的 kFallback 项，求和"（以前只算 default 皮肤）。
* **渲染器**：每个实例自己的当前皮肤；`spine.skin` 换肤后按 (动画, 皮肤) 重算自动排布的包围盒
  （default + 当前皮肤的全部可画项）。`SpineGpuSamplePoseSkinned()` 是带皮肤的无状态采样。
* **已知限制**（都不进容器、OSGame 里不存在）：① 具名皮肤里的 bbox / point 遮住 default 同名可画附件时，
  我们画 default 那条（对拍台把这种情况单列报错）；② 二进制里**空**的具名皮肤在参考侧是 NULL，
  "换成它"在参考里等于 setSkin(NULL)，容器分不出空皮肤与 NULL。
* **验证**（OSGame 781 个资产**没有一个**带具名皮肤，prefab 的 `m_InitialSkinName` 1310 个空、25 个
  "default"）⇒ 手写夹具 `Test/SpineGpuSkinFixture.h`（default / red / blue：同名不同类型、跨皮肤 linked
  mesh + 继承 deform、只在具名皮肤里的 setup 附件、具名皮肤的裁剪 / path / deform）：
  * `SpineGpuRefParityTest`：夹具进 S5 随机脚本，换肤用独立随机数流（不扰动原资产的脚本），与 spine-cpp
    逐帧比骨骼 / 槽位 / **附件来自哪个皮肤** / deform / 裁剪：64 条脚本、setSkin 593 次（含 NULL / default /
    之后 setSlotsToSetupPose）、具名皮肤帧 9 381、附件来自具名皮肤 35 681 次，0 分叉。变异 5 处
    （attachAll 不看旧皮肤 / 不查当前皮肤 / 身份不含皮肤 / 缺名字时回落 default / setup 不按当前皮肤）全部抓到。
  * `SpineGpuImportTest` +26 条手推断言（1408 → 1434）。781 资产单动画 / S5 / 多实例对拍与之前一致。
  * 编辑器（Vulkan 验证层 / DX12）：夹具 3 个实例分别 red（+setup）/ blue / blue（+setup），找不到的皮肤名
    只记一行警告，无验证层报错。

可见集解析实测：OSGame 全量第 0 个动画 t=0 时挂着附件的槽位 29 785 个能找到绘制项，
689 个找不到 —— 这 689 个槽位在 skin 0 下**一个可画项都没有**（"槽位里有别的可画项、
却没有这个名字"的是 **0** 个），即挂的是裁剪 / point / bounding box / path 这类不画的附件。
（`SpineGpuRefParityTest --uv-scan` 输出这三个数。）

#### 进场景：`SpineRenderer` 组件 + `.zspg` 资产（2026-10-23）

到这一步之前 GPU-Spine 只能用控制台驱动、画在视口像素空间。现在：

| 环节 | 做法 | 代码 |
|---|---|---|
| 产出 | `SpineCook` 把骨架 + 图集烘成 `.zspg`，**并把图集页拷到 .zspg 旁边**（运行时按页名相对容器目录找） | `Editor/ToolSuite/SpineCook/SpineCookSuite.cpp` |
| 登记 | `.zspg` 进 `AssetManager::RegistryIndexedAssetExtensions()`；类型名 `SpineSkeletonData`（只看魔数）；GUID 按路径派生（容器里没有 GUID 字段，不为它改格式） | `Runtime/Resource/Asset/AssetManager.cpp`、`Editor/AssetRegistry/AssetRegistry.cpp` |
| 浏览 / 放置 | Content Browser 可拖、无 Reimport；拖进 Scene ⇒ GameObject（Transform + `SpineRenderer`），一个撤销步骤 | `EditorScenePlacement.cpp`、`ContentBrowserHelpers.cpp` |
| 组件 | 字段：资产路径 / 动画 / loop / 初始皮肤 / time scale / `m_Scale`（骨架单位 → 世界单位，默认 0.01 = spine-unity）；全反射序列化 | `Runtime/Function/Framework/Component/Spine/SpineRenderer.{h,cpp}` |
| 朝向 | Z 向上 ⇒ 骨架立在局部 XZ 平面（x → +X、y → +Z，同 UE Paper2D），model = L2W · Rx(+90°) · diag(s)；不做 billboard | `SpineRenderer::PushTransform` |
| 渲染 | 组件持有一个**世界实例**：每 tick 推 local-to-world·diag(s) 与 dt；渲染线程 `clip_from_world = F · proj · view · model`，F 在 DX12 / Metal 上翻 y（相机投影自带 Vulkan 的 y 翻转） | `SpineGpuRenderer.cpp` 的 `WorldInstance()` |
| 加载 | `.zspg` 直接 `ReadSpineGpuContainer`；源骨架仍走"烘焙 → 写 → 读"往返 —— 两路落到同一个读端 | `SpineGpuRenderer::Impl::LoadAsset` |

生命期（踩过才定的，见 AGENTS.md §2.42）：实例只在 Tick 里建；存活判据是逻辑帧序号（不 tick 的不画、暂停定格）；
GameObject 删除走 `RenderScene::DeleteEntityByGObjectID` ⇒ `RemoveWorldInstancesOfOwner`，撤销复活由组件发现 id 失效后重建。

**实跑验收（ZGameSmoke，Vulkan + 验证层 / DX12，`--pie N --pie-stop`）**：
`spine.spawn _Generated/SpineTest/K_Bailishouyue_Spine.zspg camera 8` ⇒ GameObject #32 落在相机前 8 单位、yaw -90°；
注册表缓存里该 .zspg 的类型 = `SpineSkeletonData`、GUID 路径派生；首帧 setup 包围盒投到 NDC x [0.61, 4.14] /
y [-7.53, 0.59]、w ≈ 8（= 距离，原点在屏幕中心、人物向右上，Vulkan NDC y 向下 ⇒ 负 = 向上）；
进 PIE：编辑世界的实例 "hidden (not ticked)"、PIE 副本新建实例 #3；退出 PIE：#3 "removed"、编辑世界的 #2 "shown"。
同一次运行里 `spine.load <源骨架>` 的控制台实例照常画（38 项带 deform），两种实例共存。无验证层 / D3D12 报错。
没做肉眼确认（截图工具在本机不可用，同前）；撤销删除后的重建路径没有实跑。

**第一版不做**：GUID 引用（字段存路径，同 MeshRenderer）、Inspector 里的资产选择器、Assets 下源骨架自动 cook。
（三项都已在下文「美术可用 + 玩法可控」补上。）

#### 深度 + 面向相机（2026-10-23）

| 项 | 做法 |
|---|---|
| 画在哪 | 世界实例从 UI 层挪到 **RP1 forward / 透明 subpass**（Vulkan：`MainCameraPass::DrawRp1` 粒子之后；DX12 / Apple：`MainCameraRp1Pass::DrawForwardLightingPass` 之末）。控制台实例仍在 UI 层 |
| 深度 | 管线 depthTest LEQUAL、depthWrite 关（透明物体惯例）；`clip_from_world = proj·view·model`，与 `mesh.vert` 同一乘法、同一视口（Scene 视口 = `TryGetRenderSceneViewport`，与 `SetViewportScissor` 同口径） |
| 实例间顺序 | 按枢轴点观察空间 z 远 → 近（不写深度 ⇒ 靠绘制顺序叠） |
| 颜色空间 | 场景线性 HDR ⇒ 片元 bit5：去预乘 → pow 2.2 → 再预乘（GLSL / HLSL 同公式），随后与透明网格一样经 tonemap |
| 两层隔离 | 每帧缓冲 / 合包 / 描述符集 / 管线按层各一份（`frames[2]` / `batch[2]` / `pipelines[2][4]`）；同一帧 RP1 先录世界层、RP2 后录 UI 层，共用一份会让后者覆盖前者已引用的数据 |
| 公告板 | `SpineRenderer::m_Billboard`：`none`（GameObject 旋转）/ `full`（平行相机平面，默认）/ `vertical`（绕世界 Z，人物直立）。渲染线程用当帧相机 view 矩阵第 0 / 1 行当右 / 上；组件只推枢轴与带符号缩放（lossy x / z ⇒ 负缩放 = 镜像保留） |

**验收（RenderDoc 抓帧 + qrenderdoc python 回放）**：
* Vulkan：68 个世界层绘制在 "Forward Lighting" 里，深度附件 D32 绑定，管线 LEQUAL / 不写；post-VS z/w = 0.9876（w = 8 = 距相机），
  与按投影手算一致；场景前景物体（玩家假人，深度 ≈ 0.88）所在像素上 Spine 的片元 **depthTestFailed = True**，空处 = 通过并写色。
  回放 RT：卡面在场景里、假人挡在卡面前；最终帧里卡面朝向与天空 / 地面一致（缩略图整体上下翻是 swapchain 呈现约定，
  天空 / 地面一起翻）。验证层 0 报错。
* DX12：68 个绘制在 "Colour Pass #2 (1 Targets + Depth)"，管线 LEQUAL / 不写，post-VS 与 Vulkan 逐值相同；但该后端此刻
  GBuffer 根本没写深度（整张 1.0，抓帧在 Colour Pass #1 就是 1.0）⇒ 遮挡在 DX12 上目前**看不到**，这是 DX12 延迟管线的既有状态，
  与 Spine 无关。Metal 未编译验证（同前）。
* 公告板：编辑器相机无俯仰时 full ≡ vertical，`vertical` 的分支没有被画面覆盖到（代码路径简单，见 `BillboardModel`）。

#### 美术可用 + 玩法可控（2026-10-23）

| 项 | 做法 | 代码 |
|---|---|---|
| 单一加载路径 | 源骨架 / `.zspg` 的识别、cook、读、元数据（动画名 + 时长、皮肤、骨骼 / 槽位数）全在一处；渲染器、SpineCook、自动 cook、组件共用 | `Render/Spine/SpineGpuAssetFile.{h,cpp}` |
| 自动 cook | 启动扫 content 根，过期（源或图集比 `.zspg` 新）就 cook 到同目录 `X.zspg`；运行中文件监视 + 300 ms 去抖；`.tmp` → rename 原子替换；cook 完通知组件重建实例 | `Editor/AssetPipeline/SpineImporter/SpineAutoCook.{h,cpp}`、`EditorAssetManager` |
| GUID 引用 | 组件存路径 + GUID；路径失效按 GUID 在 registry 里找回并改写路径；registry 刷新 / 重扫保留已有 GUID（`.zspg` 的 GUID 是路径派生的，重派生 = 改名后丢） | `SpineRenderer::ResolveSkeletonPath`、`AssetRegistry.cpp` |
| Inspector | `.zspg` 拖放槽、加载状态、GUID、Animation / Initial Skin / Face Camera 下拉 | `ZSlateInspectorWindow::BuildSpineRendererSection` |
| 播放 API | set / add / setEmpty / addEmpty / clearTrack(s) / setMix / setSkin / timeScale + 当前动画 / trackTime + 元数据，全部 ZFUNCTION ⇒ TS 可调；实例未建时进队补发 | `SpineRenderer.{h,cpp}`、`SpineGpuRenderer::Request*On` |
| 事件 | start / interrupt / end / complete / dispose（spine-cpp 3.8 语义，逐条对拍），渲染线程攒、组件 tick 取走（晚 1~N 帧）；TS 轮询 `GetEvent*`，C++ `SetEventListener` | `SpineGpuAnimationState`、`SpineGpuRenderer::DrainWorldEvents` |

**验收（ZGameSmoke，Vulkan，`--cvar` 无人值守）**：删掉 `.zspg` 后启动 ⇒ `SpineAutoCook: 1 cooked (17.7 ms)`；
`spine.spawn <cook 产物> camera 8 0.003 script=Scripts/SpineEventProbe.ts` + 另一个 `.zspg` 的 spawn；
TS 探针拿到元数据（idle_01 = 0 s、idle_02 = 5.333 s、1 皮肤）与事件序列：f3 `start/interrupt/end/dispose idle_01`
（从未 apply 就被替换）→ `start idle_02`，idle_01（0 s）每帧 complete，idle_02 混出时 complete、混完 end；
收到首个 complete 后 TS 调 `SetTimeScale(1.5)` 生效。`exec.after 120 asset.move …/K_Bailishouyue_Spine.zspg …/Hero.zspg`
⇒ 进 PIE 时副本 `moved -> '_Generated/SpineTest/Hero.zspg' (by GUID …)` 并在新路径建实例；PIE 里脚本实例重新拿到同一事件序列。
touch 源骨架 ⇒ `re-cooked` + 组件 `was re-cooked, reloading`。Inspector 区段只做了编译 / 构造验证，没有肉眼看。

---

## 16. 用户事件（v16，2026-10-23）

Spine 的"用户事件"是动画里挂的**回调点**（脚步声、命中判定、特效触发）。v9 起容器里只有
`kHasEventTimeline` 一位（"有事件"这件事本身要能被看见），事件**不烘焙** ⇒ 运行时一个事件也
收不到。v16 把事件定义 + 事件时间轴真正落进容器。

### 16.1 布局（chunk 23）

```
SpineGpuEventChunkHeader     16 B
  event_count   u32 @0
  string_count  u32 @4   ≥ 1（第 0 条恒为空串）
  blob_size     u32 @8   字符串 blob 字节数（不含补齐）
  reserved      u32 @12  写 0
SpineGpuEventData[]          28 B/条 × event_count
  name         u32 @0   字符串池下标（非空：参考实现拒空名）
  int_value    i32 @4
  float_value  f32 @8
  string_value u32 @12  字符串池下标（0 = 空）
  audio_path   u32 @16  字符串池下标（0 = 无音频）
  volume       f32 @20
  balance      f32 @24
u32[]                        4 B/条 × string_count（每条在 blob 里的字节偏移）
blob                         blob_size 字节，UTF-8，每条 '\0' 结尾；整体补齐到 4 B
```

字符串池**自带在 chunk 23 里**，不往 chunk 1 塞第 10 组 —— chunk 1 的"最后一组靠剩余切分"
已经用掉了，再插一组就得再改一次切分规则。池子按**下标**引用（不是 chunk 1 那种字节偏移），
**第 0 条恒为空串**：`string` / `audioPath` 的缺省就是 0，不需要哨兵。

**没有事件时这个 chunk 根本不写**（与 chunk 9 / 18 / 19 同规矩，不是写个空块）。

### 16.2 事件时间轴（type 7）

第一次真正进 chunk 12。帧 = `time`（1 float/帧，在 chunk 13 里），载荷 = 每帧一条
`SpineGpuEventKey`（24 B，在 chunk 16 里）：

```
event         u32 @0   → chunk 23 的事件定义下标
int_value     i32 @4
float_value   f32 @8
string_value  u32 @12  → chunk 23 字符串池下标
volume        f32 @16
balance       f32 @20
```

读端校验 `event < event_count`、`string_value < string_count`（越界 = 读到别人的事件，
不报错只是触发错东西）。

**语义逐行照抄 spine-cpp 3.8**（转录时最容易抄错的三条）：

| 规则 | 出处 |
|---|---|
| `EventData`：name / int / float / string / audioPath；**只有 audioPath 非空时**才读 volume / balance，否则是构造函数的 1 / 0 | `SkeletonBinary.cpp:327-347`、`SkeletonJson.cpp:673-694` |
| `Event`（一帧）：int / float / string **逐帧覆盖**（json 缺省取 EventData 的值；二进制 int / float 每帧必有，string 有 freeString 位）；volume / balance 同样**只在 EventData 带音频时**逐帧读 | `SkeletonBinary.cpp:1084-1103`、`SkeletonJson.cpp:1176-1194` |
| ⚠️ 逐帧缺省的 volume / balance 是 **Event 构造函数的 1 / 0，不是 EventData 的** —— 参考实现在这里**不继承** | 同上（这是最容易顺手写成"继承"的一处） |
| `audioPath` 不逐帧：`Event::getData().getAudioPath()` | — |
| 事件时间轴每帧末尾是否多 2 个 float，**取决于对应事件有没有 audioPath** | `SkeletonBinary.cpp:1098-1101` ⇒ events 段不能跳过 |
| **animation duration 把最后一个事件的 time 算进去** | `SkeletonBinary.cpp:1106`、`SkeletonJson.cpp:1197` |

最后一条是 v16 的"静默错"来源：v15 及以前不烘事件 ⇒ duration 少算 ⇒ 事件在动画末尾之后的
资产 **complete 时机错、不报错**。

### 16.3 派发语义（`SpineGpuAnimationState`）

照抄 `AnimationState` 的排队规则（不是"扫到就发"）：

1. `apply` 期间**收集**（`CollectEventTimeline`），按参考的循环回绕语义（同一 time 上有多帧时
   回退到**第一帧**，不是最后一帧）；
2. `queueEvents` 把队列切成"complete 之前"与"complete 之后"两段 ⇒ 一帧里先发事件、再发 complete；
3. **mixingFrom 只在 `mix < eventThreshold` 时收集**，且**只在 `to->mixDuration > 0` 时排队**
   ⇒ 没有混合的切动画不会把旧动画的尾巴事件再发一遍；
4. 派发后 `m_Events.clear()`。

`eventThreshold` 按 `(instance, track)` 设置：`SpineGpuRenderer::RequestEventThresholdOn()`
（走 `kEventThreshold` op，作用在轨道**尾**那条 entry 上），组件侧 `SetEventThreshold(track, th)`。

### 16.4 对外接口

| 层 | 接口 |
|---|---|
| C++ 组件 | `SpineRenderer`：`SpineAnimationEvent` 带 `user` 字段；`SetEventListener` |
| 脚本（TS / C#） | `GetEventName/GetEventInt/GetEventFloat/GetEventString/GetEventAudioPath/GetEventVolume/GetEventBalance/GetEventTime`、`SetEventThreshold(track, th)`、`GetUserEventCount/GetUserEventName` |

### 16.5 验收

| 项 | 结果 |
|---|---|
| OSGame 全量（783 资产）事件对拍 | **111 266 条逐条一致**（用户事件 4 458 + 调度事件 106 808）。v15 基线是 101 369 条 / 用户事件 0 ⇒ 多出的 5 439 条调度事件**全部**来自 duration 修正（complete +1 957 / start +1 043 / end +879 / dispose +893 / interrupt +667），差额可逐类对上 |
| 手算夹具（`SpineGpuEventFixture.h`：step / sfx / blank / mute 四类事件 × walk / late / solo / burst 四条动画） | `SpineGpuImportTest` 45 条手算检查，1491 / 1491 断言通过 |
| 变异测试 | 去掉 `mix < eventThreshold` 判据、把事件全放到 complete 之后 ⇒ 两条都被抓到 |
| C# 往返 | `events.zspg` C++ 写 → C# 读 → C# 写，逐字节相同 |
| 端到端 | spineboy 自动 cook + TS 探针收到用户事件（含 string / int / float / audio / volume / balance） |

---

## 17. 性能基准（CPU 侧，2026-10-23）

**测的是"每实例每帧的 CPU 成本"，不是 GPU 时间**；参考侧是 spine-cpp 3.8 的 **CPU 蒙皮**路径
（`AnimationState::update/apply` + `Skeleton::updateWorldTransform` + 顶点生成）。

```
cmake --build build --config Release --target SpineGpuBenchmark
build/engine/Source/Runtime/Function/Render/Spine/Test/Release/SpineGpuBenchmark.exe \
    G:/OSGame/Client_proj/Project/Assets --frames 360 --csv build/spine_bench.csv [--top 8] [--limit N] [--verbose] [--split]
```
（⚠️ **目录是第一个位置参数**，必须在 `--frames` 这些选项**前面**：写在最后会被当成没给目录，
输出"没有可测的资产（跳过 0）"。）

目标 `EXCLUDE_FROM_ALL`、**不进 ctest**（是测量，不是断言），且不链 `ZRuntime`
（只编 Spine 核心几个 .cpp + 参考实现 ⇒ 不用把整个引擎编一遍 Release）。
⚠️ `SpineReference` 按 config 改名（`OUTPUT_NAME SpineReference_$<CONFIG>`）：全工程的
`CMAKE_ARCHIVE_OUTPUT_DIRECTORY` 被统一设成同一目录（根 `CMakeLists.txt:56-60`），
不改名的话 Release 的基准会把 Debug 的 `.lib` 覆盖掉，回来编对拍就 LNK2038。

**口径**：每个资产各放 1 个实例，360 帧 @60Hz，取中位数 / p90 / 合计。

| 段 | 我们 | 参考 |
|---|---|---|
| anim | `AnimationState::Update + Apply` | `AnimationState::update + apply` |
| pose | 约束 + `UpdateWorldTransform` | `Skeleton::updateWorldTransform` |
| geo | 裁剪 + 攒批（写 GPU 要的那几个字节） | 顶点生成 + 裁剪（在 CPU 算顶点） |
| upload | 每帧上传字节 | 6 × f32/顶点 × 顶点数 |

我们的**静态显存**（骨骼 / 影响流 / 顶点，一次性上传、同资产实例共享）**不进每帧**；
我们 **0 个 CPU 顶点**（蒙皮在 VS），参考侧每帧在 CPU 算顶点 —— 这是两张表差异的主轴。

**结果（OSGame 781 个资产 / Ryzen 9 9950X / MSVC Release，2026-10-23，修完每帧分配后、机器最闲的那一次）**：

| 段 | 我们 中位 | 我们 p90 | 合计 | 参考 中位 | 参考 p90 | 合计 | 参考/我们 |
|---|---|---|---|---|---|---|---|
| anim | 3.49 µs | 9.57 | 3793.6 | 2.08 µs | 6.60 | 2664.9 | **0.70x** |
| pose | 0.87 µs | 2.37 | 963.8 | 0.61 µs | 2.38 | 793.7 | 0.82x |
| geo | 0.75 µs | 2.31 | 3020.7 | 4.80 µs | 14.25 | 7072.7 | **2.34x** |
| **total** | **5.45 µs** | 16.36 | 7778.1 | **8.04 µs** | 23.47 | 10531.3 | **1.35x** |
| upload | 5.53 KB | 17.80 | 6989.7 | 17.29 KB | 49.69 | 18310.1 | **2.62x** |
| CPU 顶点 | 0（裁剪项 69732） | 0 | 69731.5 | 666 个 | 1873 | 711136.3 | **10.2x** |

（**2026-10-23 之二**：deform 热循环加 `__restrict` 之后同一张表变成 anim **0.82~0.83x**、
total **1.45~1.46x**，其余各行在噪声内没动 —— 见 §17.5。上表 anim / total 是加之前那次。
**2026-10-29 之三**：把剩下能加的 `__restrict` 全扫了一遍（末帧分支、`alpha != 1` 分支、
`time < FirstTime()`、`ClipTriangles`），只值 **~0.3%**，在噪声边缘 —— 见 §17.6。）

⚠️ **绝对值漂得厉害，只能看比值。** 同一份二进制连跑三次（相隔几分钟、中间没有任何改动）：
墙钟 **10.6 / 10.7 / 11.0 s**，我们 total 中位 **5.45 / 5.50 / 5.53 µs**，参考 **8.04 / 8.18 / 8.20 µs**
—— 两边一起慢，所以比值稳得多：**total 1.35 / 1.37 / 1.37x**、**anim 0.70 / 0.71 / 0.71x**、
**geo 2.34 / 2.36 / 2.41x**、**upload 恒 2.62x**（上传量是算出来的，不是测出来的，所以它不漂）。
⇒ 拿这份数字跟以后比之前，**先连跑三次取最闲的那次**，别拿单次绝对值下结论。

**修每帧分配前后（同一台机器、同一份口径）**：anim **0.55x → 0.70x**（我们慢 1.8x → 慢 1.43x）、
total **1.16x → 1.35x**；pose 与 geo 在噪声内没动（0.81→0.82、2.21→2.34）。

我们的静态显存：中位 33.7 KB / p90 97.7 KB / 合计 35.1 MB。
折算（按最闲那次）：1 ms 单线程预算 ≈ 我们 100 个实例 / 参考 74 个；100 个实例 @60Hz 上传 ≈
我们 52.4 MB/s / 参考 137.4 MB/s。逐资产明细在 `--csv`（含骨骼数 / 槽位数 / ik/tf/path 约束数，
以及两边的稳态每帧分配次数 / 字节）。

**结论与已知差距**：

* **geo 快 2.2x、上传少 2.6x** 是这套格式的立身之本：每帧只搬骨骼矩阵与少量动态数据，
  顶点不动、也不重算 —— 参考侧每帧要在 CPU 生成 666 个顶点（中位）再传上去。
* **total 我们更快（1.35x）**，但赢面比"应该有的"小：anim 段我们慢 1.43x 把它吃回去一大半。
* ⚠️ **anim 仍是已知差距**（下表那次 0.70x；加 `__restrict` 后 **0.82x**，见 §17.5；§17.6 又扫了
  一遍剩下的点位，只值 ~0.3%）。已定位并修掉的
  三处每帧分配见下节；**剩下的已经按时间轴类型逐项测过**，见 §17.2 —— 结论是"摊在每条时间轴
  上的 ~11 ns"，其中最大的一块（deform）是本格式的**设计取舍**，不是加多了的代码。
* pose 0.82x：更早一轮已经修掉一个大头 —— `PoseSolver` 里那个 `getenv` 的 debug 开关是
  **每帧每条 path 约束调一次**（单条 ~5 µs，Diaochan_qian 的 pose 一度 52 µs），
  改成 `static const bool` + `thread_local` scratch 后：中位 1.61 → **0.94 µs**、
  p90 9.43 → **2.61 µs**、合计 3607 → **1044 µs**（3.5x）。剩下的是逐骨骼解算本身，
  我们与参考做的是同一套迭代求解器。
* 最重的资产（`Ui_LiBaiLongTouXiang`，59 骨 / 38 槽 / 12 条 transform 约束）
  我们 872.92 µs vs 参考 965.26 µs —— 我们更快，但**不是逐个资产都赢**：同一张表里
  `K_Zhongkui_Spine`（38 骨 / 93 槽 / 3 条 transform）我们 136.62 vs 参考 92.54 µs
  （我们慢 1.5x）—— 它输在 pose（2.18 vs 1.13）与 geo（105.2 vs 99.2 KB）。
  逐资产的反例在 `--csv` 里查，别只看中位数。
  这类重资产两个实现都在 0.1~1 ms 量级，一帧放不下几个，得靠 LOD / 降帧。

### 17.1 每帧堆分配：三处根因与修法（2026-10-23）

基准里加了一个探针：替换全局 `operator new`，只在**最后一小段帧**开计数（跳过"首次扩容"），
数出来的就是稳态。修之前（120 个资产）**合计 589 次 / p90 1.3 次每帧**；修完全量 781 资产
**合计 284 次 / 中位 0 / p90 0.3 次每帧**（spine-cpp 那边 11 次）。

三处根因 —— 全是"**不报错、不画错，唯一表现就是慢**"的那类，只能靠断言挡住：

| # | 位置 | 每帧在分配什么 | 修法 |
|---|---|---|---|
| 1 | 曲线求值 `SpineGpuCurvePercentAt` | 每次求值都**重建** 18 个 float 的前向差分表，外加 O(段数) 的段号扫描 | 烘焙 / 读容器时 `SpineGpuBuildCurveCache()` 展开成 `bezier_tables[]` + `curve_bezier_index[]`（`SpineGpuAnimSolver.{h,cpp}`），求值变 O(1)；**缓存缺失直接报错**，不悄悄退化成慢路径 |
| 2 | deform 展开（`SpineGpuSkeletonState.cpp`） | 每条 deform 时间轴每帧 2~4 个局部 `std::vector<float>`（setup / 前一帧 / 后一帧展开） | `thread_local DeformScratch` 复用，`assign` 保留容量 ⇒ 逐位等价，只省掉堆分配 |
| 3 | 裁剪几何（`SpineGpuClipper.cpp`） | 每个被裁槽位每帧 4 次：世界坐标 / UV / 三角形 / 拼错误信息的 `std::string`；`SpineGpuTriangulator::Decompose` 另外每帧 new 一整个 `vector<vector<float>>` | 前三个改 `thread_local` scratch；拼串改 `ClipLabel{const char*, uint32_t}`（**只在失败路径**才真正拼 —— `"clip: 绘制项 " + std::to_string()` 超过 SSO ⇒ 每帧一次堆分配）；`Decompose` 改复用成员 `m_Polygon` / `m_PolygonIndices` + `flush()`，`SpineGpuSkeletonClipping::m_ClippingPolygons` 改成指向它的**裸指针**（不再每帧拷一份） |

⚠️ **复用 scratch 踩过的坑**：`Decompose()` 忘了在开头 `clear()` 成员 ⇒ 上一帧的最后一个凸块
漏进下一帧的第一块，对拍立刻报 `clipStart 凸块数 我们 2 vs 参考 1`。凡是"成员 / `thread_local`
scratch"，**入口第一行必须是 `clear()` / `resize(0)`**。

**挡住回归的测试**：`SpineGpuHotLoopTest`（进 ctest，27 断言）。夹具故意把三条热路径都塞进去
（stepped + bezier 的 rotate、weighted mesh 上的 deform、作用域盖到那个 mesh 的裁剪附件），断言
**动画+骨架稳态 0 分配**、**动画+骨架+裁剪（三角形）稳态 0 分配**、**四边形裁剪每帧 ≤ 2 次**
（四边形会走"合并后删空块"，`resize` 真的释放空块缓冲、下一帧再补回来 ⇒ 已知残余，用预算钉住），
外加曲线缓存的表内容 / 索引映射 / 手算求值 / 缓存缺失必须报错 / 往返重建一致。

它必须单独一个 exe：这条断言要**替换全局 `operator new`**，而 `ZRuntime` 的
`MemoryManager` 已经把全局 new/delete 接管到 mimalloc ⇒ 链 `ZRuntime` 再定义一份就是 LNK2005。
⚠️ 该目标**整目标以 `_ITERATOR_DEBUG_LEVEL=0` 编译**（见 `Test/CMakeLists.txt`）：MSVC 的
Debug 迭代器调试会给**每个**容器的构造单独 malloc 一个 16 字节的 `_Container_proxy`，
不关掉的话"稳态 0 分配"在 Debug 下稳定红（实测 8 次/帧：apply 7 + pose 1），而那不是数据缓冲、
Release 下根本不存在。基准的分配计数同理 —— **只有 Release 的数字能读**。

### 17.2 anim 段差距在哪（2026-10-23 实测）

`--split` 把 anim 拆成 Update / Apply（两边同一个切法，参考侧也是 `update` + `apply`）：

| 段 | 我们 中位 | 参考 中位 | 参考/我们 |
|---|---|---|---|
| update | 0.02 µs | 0.02 µs | 0.98x |
| apply | 3.5 µs | 2.1 µs | **0.70x** |
| 每帧 apply 的时间轴条数 | 82（p90 231） | 82（p90 231） | 1.00x |

⇒ **差距 100% 在 Apply**，且两边条数完全相同（同一份脚本、同一套动画），所以可以直接按条数摊：
我们 (3.5 − 0.38) / 82 ≈ **37.7 ns/条**，参考 2.1 / 82 ≈ **26.8 ns/条**
（0.38 = 消融掉全部时间轴 apply 之后剩下的固定成本）。

按类型消融（临时宏，只给基准目标开，测完即删；基线 3.34 µs，同一份二进制）：

| 跳过的类型 | apply 中位 | 该类型成本 |
|---|---|---|
| deform (6) | 2.00 | **1.34 µs（40%）** |
| rotate (0) | 2.55 | 0.79 µs（24%） |
| attachment (4) | 2.96 | 0.38 µs |
| translate (1) | 2.97 | 0.37 µs |
| color (5) | 3.24 | 0.10 µs |

单个候选的实测（都是"消融掉这一项，看 apply 掉多少"）：

| 候选 | 成本 |
|---|---|
| `SpineGpuWrapDegrees`（rotate 的角度归一，int 截断） | 0.11 µs/帧 |
| rigid deform 每帧 `memcpy` 一份 setup 顶点（永不变化） | 消融估 0.20 µs/帧；**交错 A/B 实测只有 ~1%（见 §17.3）** |
| `GetFrames` 的调用 + 边界校验 + 一次 `SpineImportResult` 返回 | 3.2 ns/条 ≈ 0.26 µs/帧（⚠️ 2026-10-29：**已整条删除**，见 §17.8） |
| 一次 `SpineImportResult`（`bool + std::string` = 40 B，非平凡 ⇒ 走 sret 内存返回）比返回 `bool` 贵 | **1.32 ns**（微基准 2000 万次：1.50 vs 0.18 ns/次）；每条时间轴 3 次（GetFrames / ApplyX / ApplyTimeline）≈ 4 ns/条 |
| 把展开出来的 deform 关键帧按 (时间轴, 帧号) 缓存、帧号没变就不重展 | 只值 0.23 µs/帧（anim 慢 1.68x → 1.55x），命中率低 ⇒ 不值得常驻 2×deform_len 的缓存 |

**为什么 deform 是大头**：容器只存 deform 的**增量**（规格 §5.2，省 59.5% 载荷），展开就落到
运行时 —— 每帧要展开两帧再插值；spine-cpp 是**加载期**就摊成稠密帧（`DeformTimeline.cpp:182-257`
直接读 `_frames`），运行时一次都不展开。这是拿"每帧 CPU"换"体积"，不是写错了。

**结论**：anim 的 1.4 µs 差距不是一处"加多了"，是摊在每条时间轴上的 ~11 ns。要再往下压只有三条路，
都不是顺手能改的：① deform 帧改成加载期摊稠密（放弃省下的 59.5% 载荷，换回 ~1 µs/帧）；
② 不变量只在加载期校验一次，热路径不再返回 `SpineImportResult`（~0.26~0.4 µs/帧）（⚠️ 2026-10-29：**已做**，见 §17.8 —— 校验真的前移了，`SpineImportResult` 仍在返回值上但热路不再算偏移）；
③ rigid 的 setup 顶点不再每帧拷贝 —— **已做，见 §17.3**（顺带把这条的"0.20 µs/帧"这个旧估值
修正成实测的 ~1%）。要动 ①② 之前先量，别照这两条猜。

⇒ **2026-10-23 后续**：§17.2 只做了"单侧消融"，只能说"deform 最贵"，说不出它占**差距**的多少。
§17.4 用"两侧同时消融"重新量了一遍 —— deform 一条就占满 100%，而且上面 ① 那条（加载期摊稠密）
实测要付 96 MB 常驻内存，**不划算**；§17.4 给了更便宜的改法。

### 17.3 rigid 的 setup 顶点：不拷贝，直接指进顶点缓冲（2026-10-23）

**改了什么**：`ApplyDeform` 里 rigid 附件的 setup 顶点（参考实现的 `attachment->getVertices()`）
原来每帧 `resize` + `memcpy` 一份到 `thread_local` scratch 再读。改成直接把 `setup` 指进
`asset.vertex_buffer`（新增 `RigidSetupAt()`），`DeformScratch::setup` 一并删掉。

为什么能直接指：rigid 段从 `vertex_buffer` 第 0 字节起、到 `weighted_segment_offset` 结束，
段内 `kRigidVertexStride = 8` B/顶点、就是紧挨着的 `[x0, y0, x1, y1, …]`；`vertex_base`（或
`clip.vertex_offset`）乘 8 得到的字节偏移天然是 4 的倍数。原来的 `memcpy` 只是把已经在容器里
排好的字节再搬一遍。区间校验（`byte0 + len*4 <= weighted_segment_offset`）**保留在热路径**，
所以行为与改前逐字节一致。

⇒ 比"缓存一份"更好：既省了拷贝，也不用为缓存再付一份常驻内存。

**实测**（Release，781 资产 / 360 帧，`--split`；同一份二进制里用环境变量开关老/新路径，**交错**
跑 4 轮，消除机器漂移）：

| 轮 | 老路径（memcpy） 我们/参考 合计 | 新路径（直读） 我们/参考 合计 |
|---|---|---|
| 1 | 3889.1 / 2821.3 = 1.379 | 3832.9 / 2806.6 = 1.366 |
| 2 | 3822.5 / 2774.4 = 1.378 | 3778.9 / 2779.8 = 1.359 |
| 3 | 3831.8 / 2769.4 = 1.384 | 3809.1 / 2777.8 = 1.371 |
| 4 | 3924.5 / 2852.2 = 1.376 | 3745.6 / 2748.8 = 1.363 |

新路径 4/4 全优于老路径、两组取值域**不重叠** ⇒ 是真的有收益，但只有 **~1%**（anim 合计
3867 → 3792 µs，即摊到全部资产约 0.1 µs/帧）。§17 的表头比值（anim 0.70x）**没有可测的变化** ——
这个量级本来就在基准的分辨力以下。

⚠️ **方法论教训**：最初"0.20 µs/帧"是**消融**估出来的（去掉 memcpy 看 apply 掉多少），比交错
A/B 的实测值大 ~2 倍；而"改前 vs 改后各跑 3 次、跟昨天的数对比"更是完全看不出来 —— 绝对值漂
50%，两边一起漂。**低于 ~2% 的效果只有同一份二进制交错跑才量得出来**，别拿跨会话的数字比。

**测试**：`SpineGpuHotLoopTest` 新增 section 3（27 → 40 断言），把"直接指进去读"依赖的三条前提
逐条钉住 —— ① 字节偏移 4 对齐；② 加上缓冲基址后指针真的 4 对齐（`reinterpret_cast` 不是 UB）；
③ 区间整个落在 rigid 段内；④ 逐 float 读与逐字节 `memcpy` 读**完全一致**（布局必须是
`[x0,y0,x1,y1,…]`）。夹具覆盖 4 段 / 30 个 float；把偏移故意 +1 会让其中 2 段红 4 条（已验证
不是空跑）。为什么值得钉：这三条坏了 `ApplyDeform` 会**静默**读错数，只有对拍会红。

### 17.4 anim 慢 1.4x 的根源：deform 一条占满 100%（2026-10-23 实测）

§17.2 是**单侧**消融（只跳过我们这侧的某类型），量出来的是"该类型在我们这里有多贵"，
说不出它占**差距**多少 —— 因为参考侧同样要付这笔钱，两边一起贵就互相抵消了。这一节换成
**两侧同时消融**：把同一种类型在我们和参考两侧**同时**跳掉（参考侧是把该时间轴换成一个
`apply()` 为空、但 `getPropertyId()` 不变的 `NullTimeline`，不动共享的 spine-cpp 树），
A/B 两遍跑在**同一个资产上、相隔 ~14 ms** ⇒ 机器漂移抵消，直接减出来的
`差距_关 − 差距_开` 就是"该类型对差距的贡献"。

**各类型的贡献**（Release，781 资产；基准：我们 apply 合计 4233.8 µs / 参考 3130.3 µs，差距 1103.5 µs）：

| 类型 | 我们 Δ µs | 参考 Δ µs | 对差距的贡献 µs | 占差距 |
|---|---|---|---|---|
| **6 deform** | 1278.8（30.2%） | 172.8（5.5%） | **1106.0** | **100.2%** |
| 4 attachment | 510.7（12.0%） | 485.2 | 25.5 | 2.6% |
| 0 rotate | 663.7（16.6%） | 688.7 | −24.9 | −2.5% |
| 1 translate | 256.4（5.7%） | 267.0 | −10.7 | −1.1% |
| 5 color | 153.6（3.3%） | 231.8 | −78.2 | −7.3% |

⇒ **deform 一条就占满全部差距（100.2%）**，其它类型净贡献 ≈ 0（略负 = 我们反而快一点）。
所以"anim 慢 1.4x"不是"到处都慢一点"，就是 deform 一件事。

**deform 内部拆开**（我们这侧，同一个资产上 A/B 两遍）：

| 去掉的部分 | Δ µs | 占 deform |
|---|---|---|
| 整条 deform 跳过 | 1212.1 | 100% |
| ├ 解析"作用于谁" + 早退（只解析、不干活） | 214.8 | 17.7% |
| └ 展开之后的顶点工作 | 997.3 | 82.3% |
| 　 ├ 稀疏展开（memset + memcpy + setup 累加） | 239.8 | 19.8% |
| 　 ├ 展开之后的逐顶点混合循环 | 365.6 | 30.2% |
| 　 └ 其余（末帧分支 / 早帧分支 / `SetFixedSize`） | 391.9 | 32.3% |

同一件事参考侧只花 **172.8 µs** ⇒ 顶点工作这一段我们比参考贵 **~5.8×**。

**deform 的分支画像**（真正干活的 6,163,597 条次里；临时计数器，测完即删）：

- `alpha == 1`：**84.3%**；`blend == kAdd`：**0.0%** ⇒ "覆盖式、满权重"是绝对主流
- 走"末帧"分支（`time >= LastTime()`）：**26.1%**（单帧时间轴会**永远**走这条）
- rigid 附件（要逐点加 setup 顶点）：**67.7%**；平均 deform 长度 **61.6 float**（246 B）
- 进来 15,240,246 条次，只有 **40.4%** 真干活 —— 其余在"槽位此刻没挂这个附件"上早退；
  裁剪附件的 deform 只有 1392 条次，**可忽略**

**为什么我们比参考贵**：参考读文件时就把每帧摊成稠密数组（`DeformTimeline.cpp:182-257`），
运行时 `alpha == 1` 就是一次 `memcpy`、插值就是两次稠密读 ⇒ 每 float 约 **3 次**内存访问。
我们为了省 59.5% 的载荷只存增量（§5.2），于是每帧要 memset 两份 full 数组 + memcpy 稀疏段 +
逐点加 setup（rigid 还占 2/3）再插值 ⇒ 每 float 约 **13 次**。差的就是这 4 倍的访存。

**改进方案（按性价比排序，都还没做）**

| # | 改法 | 代价 | 预估收益 |
|---|---|---|---|
| ~~P1~~ | ~~**融合快路径**：`alpha == 1` 且是覆盖语义（kFirst/kReplace，84.3%）时不再物化 prev/next 两份稠密数组，一趟直接从"稀疏帧 + setup"写进 `slot.deform`：`v[i] = lerp(sparse_prev(i)+setup(i), sparse_next(i)+setup(i), percent)`，`sparse_at(i)` 只是一次区间比较。末帧分支（26.1%）同理退化成一趟直写 | **0 内存、0 格式改动** | ~~顶点工作 997 → ~400 µs~~ **❌ 已做并实测否决：访存 8.01 → 3.18 次/float，却慢 1.7~3.5%**（见 §17.5） |
| P2 | 解析 + 早退（214.8 µs）：每帧 42.98 条 deform 进来、只有 ~17 条真干活。烘焙期给每条 deform 时间轴预解析 `{slot_index, attachment_id, deform_parent}` 三元组放进连续数组（`SpineGpuRuntimeData` 那层派生数据），运行时从"追 3 张表的依赖加载"变成"比 3 个连续 u32"；再配 per-slot 的 attachment 版本号整组跳过 | 派生数据，少量内存 | −80~150 µs |
| P3 | "其余"里那 391.9 µs 还没拆开 —— 先加计数器量 `frame_count == 1` 的比例（单帧时间轴会永远走末帧分支），若是大头就加一条"单帧直写"快路径 | — | 待测 |
| P4 | **载入期稠密化**（照抄参考实现的做法）：容器仍存增量（磁盘不变），加载时摊成稠密帧 | **实测 100,293 条 deform 时间轴全量稠密化 = 96 MB**（平均 10.1 帧/条 × 61.6 float，约 123 KB/资产）；只稠密化实际播放的那部分 ≈ 34 MB | 等价于参考侧 ⇒ 差距 ~0，但内存代价明显 |

⇒ ~~先做 **P1**~~：**P1 已经做了，实测是负收益（§17.5）** —— 省访存不等于省时间，这段不是带宽
受限。现在真正的下一步是 **P2 / P3**；真要拿 P4 的效果（每 float 3 次访存）必须先让那三趟循环
保持可向量化。§17.5 里那条 `__restrict`（+3.0~4.7%，anim 0.70x → 0.82x）才是这一轮真正的收获。

⚠️ **踩过的坑**：中间有一版把"参考侧 deform 也要 1026 µs"写进了结论 —— 那是把临时装置的一列
打印读错了（那一列是"我们正常 vs 参考跳过之后"的差，不是参考自己的 Δ）。**凡是两侧对比的
临时装置，打印出来的每一列都要先确认它是哪两个量相减**，否则结论会整个反过来。

### 17.5 deform 热循环：访存减半反而更慢，`__restrict` 才是对的（2026-10-23）

**P1（融合快路径）做完了，实测打脸。** 临时开关 `ZENGINE_SPINE_DEFORM_FUSED=1`（测完已删）：
α==1 且覆盖语义（84.3%）时不再物化 prev/next 两份稠密数组，改成一趟从"稀疏帧 + setup"直写
`slot.deform`。访存次数**真的降下来了**：**8.01 → 3.18 次/float**（−60%，临时计数器实测）。
同一个资产上 A/B 两遍（间隔 ~14 ms ⇒ 漂移抵消）：

| 轮 | 变体 0（老路径） | 变体 1（融合） | 差 |
|---|---|---|---|
| 1 | 4122.4 µs | 4248.5 µs | 融合**慢 3.1%** |
| 2 | 4064.0 µs | 4208.2 µs | 慢 3.5% |
| 3 | 3893.5 µs | 3959.6 µs | 慢 1.7% |

**为什么**：逐元素都要判 `i` 落不落在稀疏区间里 ⇒ 循环带分支、只能标量跑；而"展开"那三趟
（`assign` 补 0 / `memcpy` 稀疏段 / 逐点加 setup）每一趟都是**连续直线**循环，MSVC 能向量化。
省下来的是访存，亏掉的是 SIMD —— **这段根本不是带宽受限**。
⇒ §17.4 那张"每 float ~13 次访存 vs 参考 ~3 次"的账**算对了**，但把"访存少"直接当成"更快"是错的。

**真正有效的是 `__restrict`**（变体 2）：`vertices`（槽位的 deform 向量）/ `prev` / `next`
（thread_local scratch）/ `setup`（指进 `asset.vertex_buffer`）四块数据互不相交，但不写出来
MSVC 不敢向量化 `v[i] = pv[i] + (nv[i] - pv[i]) * percent` 和 `dst[q] += rs[q]`。
访存次数**不变**（7.89 vs 8.01，差异只是计数口径）：

| 轮 | 变体 0（老路径） | 变体 2（老路径 + `__restrict`） | 差 |
|---|---|---|---|
| 1 | 3911.1 µs | 3793.6 µs | **快 3.0%** |
| 2 | 3868.6 µs | 3687.0 µs | **快 4.7%** |

⚠️ 这两轮里变体 0 都是"第二遍"（紧接在变体 2 之后跑，缓存更热）⇒ 3.0~4.7% 是**保守下界**。
全量基准的比值也印证：anim **0.70x → 0.82 / 0.82 / 0.83x**（连跑三次），total 1.35x → 1.45~1.46x。

**落地**：`ExpandDeformFrame` 的 setup 累加 + `ApplyDeform` 主路径的插值循环各加一句
`__restrict`；融合快路径的代码、临时开关、访存计数器**已全部删除**（结论留在这一节）。
顺带把帧数据的解码抽成 `GetDeformFrameRun()`（`ExpandDeformFrame` 现在复用它）。

**验收**：对拍 781 资产 **PASS / 0 分叉**（deform 逐 float 274,232,712 个一致）；
`ctest -R SpineGpu` 4/4；每帧 0 分配的断言（HotLoopTest）不受影响。

**下一轮**：P1 已否 ⇒ 轮到 **P2（解析 + 早退 214.8 µs）和 P3（"其余"391.9 µs 还没拆开）**；
P4（载入期稠密化 96 MB）仍然只在 P2/P3 做完还差得远时才考虑。

> ⚠️ **2026-10-29 修订**：这两个数是非交错消融的估计值，偏大 ~2 倍。改用**逐帧交错**重测后
> （`engine/Libraries/GPUSpine/doc/PERF_ACCEPTANCE.md` §1.4）：**解析 + 早退只有 102.5 µs
> = 我们 anim 的 3%**；deform 对 anim 差距的贡献从 100.2% 降到 **40%**；两侧 deform 在 anim 里
> 的占比几乎相同（36.7% vs 36.0%）⇒ **anim 是整体慢 ~20%，不是 deform 特别差**。
> **先按 §1.4 的方法重测再排优先级**，别照本节的旧数字开工。
另外 —— **`__restrict` 这一招大概率不止 deform 一处**：pose / geo 里凡是"两块 `float*` 参与、
编译器无法证明不重叠"的逐元素循环都该扫一遍（同一份二进制 + 开关 + 交错 A/B 的老规矩）。

### 17.6 全模块 `__restrict` 扫描：剩下的点位只值 ~0.3%（2026-10-29）

上面那句作业做完了。范围 = `Render/Spine/` 全部 9 个 .cpp；筛选用"**有没有第二块指针**"+
"**循环体内有没有 `break` 之类的边界检查**"（有的话 MSVC 本来就不向量化，加 restrict 也是白加）：

| 位置 | 循环 | 结论 |
|---|---|---|
| `ApplyDeform` 主插值（`alpha==1` ⇒ 73.9%） | `vr[i] = p + (nvr[i]-p)*percent` | §17.5 已加 |
| `ExpandDeformFrame` 的 setup 累加（rigid 67.7%） | `dst[q] += rs[q]` | §17.5 已加 |
| `ApplyDeform` **末帧分支**（26.1%） | 6 条（kSetup/kAdd × rigid/weighted、kFirst/kReplace） | **本次加** |
| `ApplyDeform` `alpha != 1` 分支（15.7%） | 4 条（kSetup/kAdd × rigid/weighted、kFirst/kReplace） | **本次加** |
| `ApplyDeform` `time < FirstTime()` 的 rigid 循环 | `v0[i] += (s0[i]-v0[i])*alpha` | **本次加**（罕见） |
| `SpineGpuClipper::ClipTriangles` 输出循环 | `cv` / `cuv` / `co` | **本次加**（只在裁剪时跑） |
| `PoseSolver` 的 path / mesh 顶点循环 | 体内有 `vi+1 >= send` 之类 `break` | **不加**：本来就不向量化 |
| `AnimSolver` 的贝塞尔表 / 帧查找 | 单指针 + 定长 9 次 | **不加**：没有第二块指针 |

**量法**：同一份源码树编两份二进制（**只差这次的 `__restrict`**），交错跑 base / restrict / base / …：

| 轮 | base 我们 anim 合计 | restrict 我们 anim 合计 | 差 |
|---|---|---|---|
| 1 | 4593.0 µs | 4584.7 µs | **−0.18%** |
| 2 | 6408.6 µs | 3647.7 µs | 作废（整轮绝对值翻倍，机器被别的事占着） |
| 3 | 3320.2 µs | 3308.8 µs | **−0.34%** |

⇒ **第二批只值 ~0.3%**，方向两轮一致，但**明显低于我们"2% 才可信"的阈值**，属于"可有可无"。
（比值那一行更容易被误导：round 1 是 0.79 → 0.82，看着像涨了 4%，其实是**参考侧**自己在
3636.8 → 3767.9 地漂；看我们侧的绝对值才干净。**比值只在两侧同进程相邻跑时才抵消漂移。**）

还是留下的理由：这几条 `__restrict` 断言的都是**真命题**（注释里逐条写明了每块数据的来源），
零运行成本、零行为差异，且把"这块数据不会和那块重叠"这个事实固化进了代码。

⚠️ **踩到两个坑，都和 `__restrict` 无关但会被它放大成野指针**：
1. `last` / `prev` / `next` 的 `.data()` **必须**在 `ExpandDeformFrame` **之后**取 —— 它里面是
   `full.assign()`，容量不够就重分配，提前取的指针会悬垂（预热期 / 遇到更长的 deform 时间轴时必炸）。
   `assign` 只在容量不足时重分配，所以稳态"看起来没问题"，但这恰恰是最难复现的那类 bug。
2. 同一个对象**只能有一份** `__restrict` 指针。原来主插值那个块里又声明了一遍 `v`/`pv`/`nv`
   （指向 `vertices` / `prev_v` / `next_v`），和外层 `vr`/`pvr`/`nvr` 撞车 ⇒ 已改成复用外层。
   两个 restrict 指针指向同一块内存，"基于 restrict 的访问"这条约定自己就破了。

**验收**：对拍 781 资产 **PASS / 0 分叉**（deform 逐 float 274,232,712 一致；裁剪也被覆盖 ——
`clipTriangles` 逐位 359,772 次，所以 `ClipTriangles` 那处是真跑到了，不是空转）；
`ctest -R SpineGpu` **4/4**。

**下一轮**：`__restrict` 这条线到此为止（剩下的都是噪声量级）。回到 §17.4 的 **P2 / P3**
（⚠️ 2026-10-29 修订：数字已按 `PERF_ACCEPTANCE.md` §1.4 下修 —— 解析 + 早退只有 102.5 µs = anim 的 3%，
新 1 号目标是"anim 整体比参考慢 20%"这件事）；P4（载入期稠密化 96 MB）仍然只在 P2/P3 做完还差得远时才考虑。

---

---

### 17.7 多实例与内存（2026-10-29）

§17 的主表只回答了"1 个实例、CPU 时间"，而 §0 的收益主张是 **CPU + 内存**，另一半没量过。
这一节补上：基准加了 `--instances N`（每资产同时跑 N 个实例，两边都按"一帧把 N 个实例拼进
同一个合包 / 同一个动态 VB"的方式做）和**常驻字节**统计。

⚠️ **量法修正**：spine-cpp 的分配**不走 `operator new`** —— `SpineObject::operator new` 与
`SpineExtension::alloc` 最终落到 `DefaultSpineExtension::_alloc` → `::malloc`。所以只替换
`operator new` 数不到它（2026-10-23 那版"spine-cpp 稳态分配合计 11 次"其实只数到了基准
自己的 `std::vector`，spine-cpp 内部一次都没数到）。现在装了一个计数的 `SpineExtension`
（`SpineExtension::setInstance`），两条路径共用同一套带头的分配器，常驻字节可以直接比。

**构建（模块已移到 `engine/Libraries/GPUSpine`，自带 CMake）**：

```
cmake -S engine/Libraries/GPUSpine -B build -DGPUSPINE_BUILD_TESTS=ON \
      -DGPUSPINE_RAPIDJSON_DIR=<rapidjson include> -DGPUSPINE_REF_ROOT=<spine-cpp 根>
cmake --build build --config Release --target SpineGpuBenchmark
build/Tests~/Release/SpineGpuBenchmark.exe G:/OSGame/Client_proj/Project/Assets [--instances N]
```

**结果（781 资产 / Ryzen 9 9950X / MSVC Release；N=1 用 360 帧、N=16 用 120 帧）**：

| 项（每实例每帧） | 我们 N=1 | 参考 N=1 | 我们 N=16 | 参考 N=16 | 参考/我们 |
|---|---|---|---|---|---|
| anim（update+apply） | 3.30 µs | 2.24 | 3.26 | 2.10 | **0.74~0.78x** |
| pose（约束+世界变换） | 0.93 µs | 0.64 | 0.96 | 0.68 | **0.85~0.86x** |
| geo（裁剪+攒批 / 顶点生成） | 0.77 µs | 5.18 | 0.81 | 5.16 | **2.28~2.30x** |
| 合计 | 5.37 µs | 8.49 | 5.35 | 8.28 | **1.40~1.42x** |
| 上传 | 5.53 KB | 17.29 | 5.58 | 17.27 | **2.57~2.62x** |
| CPU 顶点数 | 0 | 666 | 0 | 668 | **10.1~10.2x** |

* **两边都随 N 线性增长**（每实例成本与 N=1 基本一致）⇒ 上表可以直接乘 N，没有"攒批摊薄"
  这类只在某一侧成立的效应。
* ⚠️ **anim / pose 两段我们比参考慢**（0.78x / 0.86x）：我们多做了 deform 采样、槽位→绘制项
  解析、每帧裁剪作用域重算；参考侧这些成本被算进了它的 geo（顶点生成）里。赢面全部来自
  **geo 段不再在 CPU 上算顶点**（2.3x，且 CPU 顶点数 10x 少）。
* **内存（堆常驻，781 资产平均；GPU 动态部分按 3 帧在途）**：

| | CPU 共享 | CPU 每实例 | GPU 共享 | GPU 每实例/帧 | N=1 总 | N=10 总 | N=100 总 |
|---|---|---|---|---|---|---|---|
| 我们 | 229 KB | 43~47 KB | 33.7 KB | 5.5 KB | 571~576 KB | 1331~1382 KB | 8932~9442 KB |
| spine-cpp | 310 KB | 49~54 KB | 0 | 17.3 KB | 663~670 KB | 1861~1929 KB | 13 833~14 521 KB |
| 参考/我们 | 1.20x | 1.10x | — | 2.6x | 1.16x | 1.40x | 1.54x |

  ⇒ 共享与每实例两项我们都不大于参考，**任何 N 都是我们更少**；N 越大优势越大（N=100 时
  1.54x）。原因是每实例那份里参考要存一份动态顶点（每帧 17 KB 上传 × 3 帧在途）。

**v17（unorm16 权重）对这条基准没有影响**：字节数不变，只是解码除数从 255 变 65535。
它影响的是**精度**——端到端蒙皮对拍里 weighted 顶点的最大误差 **5.21 → 0.071** 骨架单位
（rigid 不变，1.2e-4），门槛随即从"只跟自己的底噪比"收紧成绝对 0.1 并接进 verdict。

**下一轮该看的**：anim / pose 两段（我们慢的那一侧）—— §17.4 的 P2（解析 + 早退）与
P3（"其余"没拆开的部分）仍然是最具体的线索。
（⚠️ 2026-10-29 再修订：P2/P3 已被 `PERF_ACCEPTANCE.md` §1.4 / §1.5 拆完并降级 ——
真问题是"**每条时间轴的 apply 比参考贵**"（48.6 vs 38.8 ns/条），不是某一类写坏了；
§1.6 已经把 `GetFrames` + 曲线查表前移进载入期，见下面 §17.8。）

### 17.8 载入期预解析：anim 快 16%（2026-10-29）

§17.2 那张表里的 "`GetFrames` 3.2 ns/条" 已经被**整条删掉**了：`GetFrames()` 与 `FrameView`
不复存在，换成 `SpineGpuRuntimeData::timelines[]`（`SpineGpuTimelineRt`，40 B/条），
在 `SpineGpuBuildRuntimeData` 里一次算好 `frames` / `entries` / `target` / `type` /
`curve` / `bezier`，并把原先每次都做的帧区间、曲线段区间、bezier 表区间检查**前移成载入期校验**。
apply 的第一件事从"算三个偏移 + 查五次边界"变成"读一条记录"。

⚠️ 副作用（写进代码注释了）：`SpineGpuRuntimeData` 现在持有**指进 `SpineGpuAsset` 内部**的裸指针
⇒ 建完表之后那个 asset 不能再让 `frames` / `curve_types` / `curve_bezier_index` 重新分配
（**移动** asset 是安全的）。另外"曲线缓存没建"这条错误从**求值期**前移到**建表期** ——
两个测试的 `Check(!r.ok)` 已跟着改成建表报错。

**实测**（两个 exe 交替跑、各 4 次、含一次反序；用"我们/参考"倍率消机器漂移）：

| 口径 | 旧 | 新 |
|---|---|---|
| anim 合计（我们/参考） | 1.293x 慢 | **1.088x 慢**（−15.9%） |
| anim 逐资产中位（我们/参考） | 1.489x 慢 | **1.268x 慢**（−14.8%） |
| total | 我们 1.383x 快 | 我们 **1.532x 快** |

代价：781 资产 CPU 共享内存 343792.6 → 353776.8 KB（**+9.75 MB**），仍 1.17x 小于参考。
完整的方法与逐次原始数字在 `engine/Libraries/GPUSpine/doc/PERF_ACCEPTANCE.md` §1.6。

**§17.8 的续（同一天，§1.7）**：热路上还剩下 `segment = cur / entries - 1` 这个**运行时整数
除法**（`entries` 是每条时间轴不同的值 ⇒ 真 `div`，20~26 周期；实际只取 1/2/3/5/8）⇒
按 `entries` `switch` 分派成常量除法（2 / 8 两档直接移位，`cur` 恒是 `entries` 的倍数 ⇒ 精确；
`default` 兜底仍是真除法 ⇒ 将来新增类型**不会算错，只是慢回去**）。
实测 **anim 我们/参考 1.088x → 1.034x 慢**（8 组配对跑 **8/8 同向**，配对内 −4.0% ~ −8.0%，
中位 −5.2%；逐资产中位口径只有 −1.7%，量化 ±0.4% 且按资产等权 ⇒ 报 −5% 是保守值）。
**⇒ anim 已经基本追平参考（1.293x → 1.088x → 1.034x 慢），剩下的 3.4% 不值得再投入。**

## 附：事实来源（已核对）

| 断言 | 出处 |
|---|---|
| rigid 分支：整块由 `slot._bone` 单矩阵变换 | `spine-cpp/src/spine/VertexAttachment.cpp:71-87` |
| weighted 分支：`_bones` 每顶点 `[n, idx…]`，`_vertices` 每影响 `(vx, vy, weight)` 步长 3 | 同文件 `:89-113` |
| deform：`vx = vertices[b] + deform[f]` | 同文件 `:114-131` |
| Region 四点 br/bl/ul/ur 来自 `_vertexOffset[8]` | `spine-cpp/src/spine/RegionAttachment.cpp` |
| 混合模式 4 种 | `spine-cpp/include/spine/BlendMode.h:35-38` |
| 骨骼世界变换 getter | `spine-cpp/include/spine/Bone.h:189-209` |
| 槽位顺序 / 骨骼顺序 | `spine-cpp/include/spine/Skeleton.h:174-180` |
| `_bones` 声明为 `Vector<unsigned short>`（3.8 判据，注释："a skeleton cannot reach 65535 bones"） | `VertexAttachment.h:79-81` |
| 3.8 deform 重做：`applyDeform(VertexAttachment*)` + `getId()` / `const int _id` | `VertexAttachment.h:66,69,86` |
| `computeWorldVertices` 两种重载（2 参 / 6 参带 stride） | `VertexAttachment.h:53-63` |
| rigid 公式精确行号（`:83` = x，`:84` = y） | `VertexAttachment.cpp:71,83-84` |
| weighted 非 deform 块 `:97-112`；deform 块 `:115-131`，`vx` 在 `:122` | `VertexAttachment.cpp` |
| 裁剪是 CPU 几何重写 | `SkeletonClipping.h:47-49`（`clipTriangles`）、`:52-54`（`getClippedVertices/Triangles/UVs`） |
| `ClippingAttachment : public VertexAttachment`（多边形与 mesh 顶点**同源** ⇒ 可复用主顶点缓冲 + 同一去重池，header 不用加字段） | `ClippingAttachment.h:38` |
| 二进制 `readVertices`：weighted 是**显式 bool**，且 `boneCount`/`boneIdx` 是 **varint**（只有 x/y/w 是 float） | `SkeletonBinary.cpp:658-691` |
| json `readVertices` 的 weighted 判据 = 数组长度 ≠ `vertexCount*2`（裁剪附件没有 uvs 可比对，只能靠这个数） | `SkeletonJson.cpp:1218` |
| OSGame 侧 spine-unity 运行时是 **3.7** | `Client_proj/Project/Assets/OSGame/3rd-Party/Spine/version.txt` |

### 二进制 `.skel` 的出处（导入器两种输入之一）

`.skel` 是 Spine 编辑器的**默认**导出格式，所以导入器必须支持它，不能只读 json。
字段顺序全部照抄 `spine-cpp/src/spine/SkeletonBinary.cpp`：

| 断言 | 出处 |
|---|---|
| 顶层：hash 串 → version 串 → width/height → `nonessential` | `:132-148` |
| ⚠️ **文件没有魔数**，开头就是 hash 串 | `:132`（传错文件不会立刻失败，会顺着 varint 读飞） |
| bones：父下标**只在 `i != 0` 时**读，且指向已读过的骨骼 | `:150-171` |
| slots：dark 四分量**无条件读**，不全是 `0xff` 才算有 dark color | `:173-193` |
| ⚠️ region 字段顺序 = rotation,x,y,scaleX,scaleY,width,height | `:530-536`（**与 json 的 x,y,width,height,rotation 不同**） |
| mesh 顺序 = path,color,vertexCount,regionUVs,triangles,vertices,hull | `:553-582` |
| `readVertices`：先一个 bool 决定 weighted；weighted 时每顶点 `[n, (idx,x,y,w)×n]` | `:658-691` |
| varint = LEB128，最多 5 字节；`optimizePositive=false` 时是 zigzag | `:467-489` |
| 字符串：varint 长度**含**结尾 `'\0'`，但结尾不落盘；长度 0 = 空串 | `:413-424` |
| 大端：`readInt` / `readFloat`（int32 按位重解释）/ `readShortArray` | `:449-458`、`:708-717` |
| 附件类型枚举 0..6（region,bbox,mesh,linkedmesh,path,point,clipping） | `include/spine/AttachmentType.h:34-42` |
| 动画里 7 段顺序：slot/bone/IK/transform/path/deform/**drawOrder**/event | `:725-1107` |
| ⚠️ 事件时间轴每帧末尾是否多 2 个 float，**取决于对应事件有没有 audioPath** | `:1098-1101` ⇒ events 段不能跳过 |
| curve：0=linear、1=stepped 无数据；2=bezier 带 4 个 float | `:1112-1127` |
| linked mesh：`setParentMesh` 拷父的 _vertices/_bones/_regionUVs/_triangles/_hullLength | `MeshAttachment.cpp:226-239` |
| linked mesh 的 UV = **父的 regionUVs × 自己的 region** | `MeshAttachment.cpp:64-92`（`updateUVs` 读自己的 `_regionU/V/…`） |
| ⚠️ `configureAttachment` 是**空函数**，region 在 `newMeshAttachment` 里就设好了 | `AtlasAttachmentLoader.cpp:120-122`、`:73-98` |
| json 里 linked mesh 有两种写法：`type:"linkedmesh"`，或 `type:"mesh"` 带 `"parent"` | `SkeletonJson.cpp:462-465`、`:538` |
| linked mesh 解析：按 skin 找父附件，找不到报 "Parent mesh not found" | `SkeletonBinary.cpp:302-323`、`SkeletonJson.cpp:649-669` |
| ⚠️ 找父附件用的 skin = `"skin"` 字段（**空 ⇒ default**），**不是** linked mesh 所在的那个 skin | `SkeletonBinary.cpp:302-303`、`SkeletonJson.cpp:653-654` |
| `applyDeform(source)` = `this == source \|\| (_inheritDeform && _parentMesh == source)` | `MeshAttachment.cpp:94-95`（基类版只判 `this == source`：`VertexAttachment.cpp:134-136`） |

### deform / FFD 的出处（容器 v4）

| 断言 | 出处 |
|---|---|
| deform 长度 = `weighted ? verts.size()/3*2 : verts.size()` | `SkeletonJson.cpp:1071`、`SkeletonBinary.cpp:976` |
| rigid 的 deform 存**绝对**局部坐标，运行时整体**替换** `_vertices` | `VertexAttachment.cpp:72-73`；构造处 `:1085`、`:1100-1104` |
| weighted 的 deform 存**偏移**，运行时逐影响**相加** | `VertexAttachment.cpp:122-123` |
| `f = skip << 1` 里的 `skip` 累加的是**影响数** ⇒ weighted 是每影响 2 float | `VertexAttachment.cpp:90-94`、`:115-127` |
| ⚠️ rigid：3.8 的 `RegionAttachment` 继承自 `Attachment`，**不是** `VertexAttachment` ⇒ deform 指向 region 时参考实现是未定义行为 | `RegionAttachment.h:46`、`SkeletonJson.cpp:1067`、`SkeletonBinary.cpp:972` |
| 运行时对非 VertexAttachment 的附件**静默不生效**（有 RTTI 检查） | `DeformTimeline.cpp:72-74` |
| deform 段结构 = { skin: { slot: { attachment: [帧] } } }；帧可缺省 `vertices`（= 回到 setup） | `SkeletonJson.cpp:1049-1115`、`:1081-1086` |
| 二进制帧：`end==0` 表示无顶点；否则读 `start` 后 `end += start`，再读 `end-start` 个 float ⇒ **落盘第二个 varint 是"个数"不是"终点"** | `SkeletonBinary.cpp:989-1018` |
| ⚠️ 帧的 `offset + 顶点数` 无边界检查 ⇒ 参考实现会**越界写** | `SkeletonJson.cpp:1092-1099` |
| 皮肤找不到时参考实现是**空指针**（json）/ **越界读**（二进制） | `SkeletonJson.cpp:1051`、`SkeletonBinary.cpp:959` |
| 附件找不到时参考实现**倒是**报错："Attachment not found" | `SkeletonJson.cpp:1061-1065`、`SkeletonBinary.cpp:966-970` |
| json 里 inheritDeform 的字段名是 **`"deform"`**（不是 "inheritDeform"），默认 1 | `SkeletonJson.cpp:572` |
| 二进制里 inheritDeform 是紧跟 parent 的一个 bool | `SkeletonBinary.cpp:594` |

### 裁剪作用域的出处（容器 v5）

| 断言 | 出处 |
|---|---|
| `ClippingAttachment : VertexAttachment`，只有一个私有的 `SlotData* _endSlot`，构造时置 NULL | `ClippingAttachment.h:38-53`、`ClippingAttachment.cpp:42` |
| json：名字取自 `"end"`，`findSlot()` 的返回值**不检查** ⇒ 槽位名写错 / 缺失时 `_endSlot` 静默变 NULL | `SkeletonJson.cpp:631-635` |
| 二进制：**先 `endSlotIndex`（varint）再 `vertexCount`**；越界报 "parse clip attachment failed!" | `SkeletonBinary.cpp:637-646` |
| `clipEnd(Slot&)` 只在 `_clipAttachment->_endSlot == &slot._data` 时收尾 | `SkeletonClipping.cpp:72-76` |
| 已经有一个裁剪在跑时 `clipStart()` 直接返回 0 ⇒ **嵌套裁剪被忽略** | `SkeletonClipping.cpp:49-51` |
| 走到裁剪附件所在槽位 ⇒ `clipStart()` 后**立刻 continue** ⇒ 该槽位不画，且**跳过**了收尾的 `clipEnd(*slot)` | `MeshGeneration/SpineMesh.h:153-159`（对比 `:257`） |
| 槽位在裁剪中 ⇒ `clipTriangles()` 重写顶点 / 三角形 / UV | 同文件 `:189-197` |
| 整条绘制指令末尾还有一次**无条件**的 `clipEnd()` | 同文件 `:259` |

### skin 的出处（容器 v2）

| 断言 | 出处 |
|---|---|
| 3.8 的 json：skins 是**对象**（皮肤名 → 槽位 → 附件），皮肤名取自**成员名** | `SkeletonJson.cpp:439` `new Skin(skinMap->_name)` |
| ⚠️ 数组形式读不了：数组成员是 `new Json(NULL)` 且**从不**赋 `_name` | `Json.cpp:402-450`（`parseArray`）对比 `:479/507`（`parseObject` 才赋 `_name`） |
| 二进制：default 皮肤是**先读的**那一个，名字写死 `"default"` | `SkeletonBinary.cpp:276` |
| ⚠️ 空皮肤**占位不对称**：default 空 ⇒ 不占下标；具名皮肤空 ⇒ 仍占下标 | `SkeletonBinary.cpp:277-283`（`skinsCount` 只在 defaultSkin 非空时 +1）、`:293-295` |
| 二进制具名皮肤数 = 单独一个 varint，不含 default | `SkeletonBinary.cpp:277` |
| 同一皮肤内 `(slotIndex, name)` 重复 ⇒ **后写的覆盖**先写的 | `Skin.h:118-120`（`addAttachment` 注释） |
| 皮肤内查附件只在**该皮肤**内 | `Skin.h:126`（`getAttachment(slotIndex, name)`） |

### 约束与执行顺序的出处（容器 v11）

| 断言 | 出处 |
|---|---|
| `updateCache()` 按 `i = 0..N-1` 扫 `ik → transform → path` 找 `order == i` 的那条；**找不到就跳过** | `Skeleton.cpp:147-175` |
| ⚠️ `order` 是**三类约束共用的**一个排名空间，不是数组下标 | 同文件（三类混在同一个 `i` 循环里） |
| `_updateCacheReset`：约束动过的骨骼要在每次遍历前复位到 setup | `Skeleton.cpp:200-211` |
| ⚠️ path 约束的 target 是**槽位**（取 `skeletonData->_slots[...]`），IK / transform 的 target 是**骨骼** | `SkeletonBinary.cpp:249`、`SkeletonJson.cpp`（对应分支） |
| json 的 IK 字段：`mix` 默认 **1**，`bendPositive` 布尔 ⇒ `? 1 : -1` | `SkeletonJson.cpp:289-292` |
| ⚠️ Spine 的 json 解析器把 `true/false` 写进 `_valueInt`，而 `getInt()` 读的正是它 ⇒ **布尔必须当 0/1** | `Json.cpp:89-91`、`Json.cpp:165`（calloc ⇒ 0）、`:173`（⇒ 1） |
| json 的 mode 串认不出时**没有 else** ⇒ 停在构造函数默认 Fixed/Length/Tangent；**缺键**时才回落 percent/length/tangent（两者不一致） | `PathConstraintData.cpp:48-50` |
| `PathConstraint::update()` 用 `_target->getAttachment()`（槽位**当前**挂的附件），取不到就 `return` | `PathConstraint.cpp:76-80` |
| ⚠️ `sortPathConstraint` 遍历**所有**皮肤的该槽位附件 ⇒ 一槽两位 path 是合法的 | `Skeleton.cpp:557-635` |
| `PathAttachment` 的 lengths 条数 = `vertex_count / 3` | `SkeletonBinary.cpp:550` |
| 3.7 vs 3.8 约束数学的 **2 处**真差异（都是 3.8 修的 3.7 bug） | `Bone.cpp:110-113`（根骨骼 `sx`/`sy` 用反）、`IkConstraint.cpp:63`（单骨骼 `&&` 优先级 ⇒ NaN） |
| ⚠️ 逐文件 diff 前必须剥掉 license 头 | 不剥则 500+ 行噪声淹掉真差异 |

**一槽两位 path 的处理是"撤回自己"的典型案例**：我最初按"重复 ⇒ 数据坏了"报错，
一次卡掉 12 个真资产（769/41 对不上 781/29 基线）。回查 `Skeleton.cpp:557-635`
发现参考实现会把**两条**都遍历 ⇒ 合法。改成两条都烘焙 + 名字进字符串表 +
`path_index` 按**名字 == setup 附件名**解析后回到基线。
⇒ 判据永远是"参考实现真的会遍历它"，不是"我觉得重复就是错"。

**`.skel` 与 json 必须产出同一个容器。** 这一条有一个硬测试钉住
（`SpineGpuImportTest` 第 9 节）：手写的 `.skel` 字节流夹具与 json 夹具内容等价，
两条解析路径各自 Bake 后**顶点缓冲逐字节相同**。二进制那边的字段顺序、大端、varint、
weighted 扁平编码只要错一处，两边就会分叉 —— 这是唯一能自动发现这类错误的手段。

**越界读的守卫**：`.skel` 既没有魔数也没有长度字段，截断文件是最容易触发越界读的场景。
测试对夹具的**每一个真前缀**都调一次解析，要求全部失败（不能"假装成功"，更不能越界读）。
