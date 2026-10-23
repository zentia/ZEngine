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
> 内存几乎不变甚至略增（静态 VB 多了骨骼槽）。收益的来源是 **共享 + 实例化**。

因此本规格的核心不是"怎么把矩阵传到 GPU"，而是**如何把"与实例无关"的部分和"与实例有关"的部分切干净**。

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
| 22 `influence_bone` | `boneIdx u16` (0-1) + `weight unorm8` (2) + `pad` (3) | 4 B | `bone.Load((influence_offset + k) * 4)`，整条一次 Load 再拆 |

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
- **权重归一化**：导入期把该顶点 `influence_count` 个 unorm8 调整到**和恰好 255**
  （余量补给权重最大的那个），运行时统一 `/ 255.0`。不要在 shader 里做除法归一化
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

> **不要用纹理传骨骼。** 纹理会额外吃显存，而本项目的痛点正是内存；
> 且纹理采样路径（归一化坐标 + 滤波）会引入与 CPU 不一致的舍入。
> 用 structured buffer / instance data。

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
            float w = weight[i] / 255.0;
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
  0x08  spine_runtime    u32   产生它的 spine-cpp 版本（3.8.xx → 30800）
        ⚠️ 必须记录：3.8 的 _bones 是 u16，4.x 是 int，布局不同
  0x0C  flags            u32   kIndex32 / kHasDrawOrderTimeline / kUsesTintBlack / kHasClipping
                               / kHasDeform（有绘制项因 FFD 走了 CPU fallback，见 §7.1）
                               / kHasEventTimeline（v9：有事件时间轴。事件**不烘焙**，
                                 但"有却不记"等于静默丢弃 ⇒ 用一位显式说出来）
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
  id=22 influence_bone (boneIdx u16, weight unorm8, pad u8) × 影响条目数，4 B/条（v7）
                       ⚠️ 21 与 22 **共用同一个条目下标**（= 权重顶点的
                       `influence_offset`）⇒ 读端必须校验两条的条目数**相等**，
                       对不上就报错（配错了对不报错，只是每个影响取到别人的数据）。
                       id 从 21 起：10..20 留给动画阶段（骨骼树 / 时间轴 / path 几何）。
```

> ⚠️ **chunk id 在 v3 重排过**：`uv_buffer` 插在 `vertex_buffer` 与 `index_buffer` 之间，
> 其后所有 id 顺移一位。读端按 **id** 找 chunk，**不许按下标** —— v2 的容器用 v3 读端
> 读会拿到 `index_buffer` 当 UV，静默错位。这也正是 `kFormatVersion` 升到 3 的原因。
>
> ⚠️ **文件里 chunk 的顺序也不是 id 顺序**：写端按
> `1, 2, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 3, 21, 22, 4, 5, 9, 6, 7, 8` 追加
> （骨骼 setup 紧挨 slot_bone；v9 的动画块 11-16 紧跟其后、v10 的槽位块 17 再跟其后、
> v11 的约束块 18-20 又跟其后；影响流紧跟
> 顶点块；9 夹在 5 与 6 之间是
> 历史顺序）。换了顺序文件照样能读，但字节不再逐字节相同 ⇒ 跨语言对拍（§12）立刻红，
> 所以 C# 端必须照抄这个顺序（见 `SpineGpuContainerIo.cs`）。

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
  **v14 绘制项 32 B（+ vertex_base / deform_parent / vertex_count），GPU mesh deform（S6，§7.1）**。
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
| C# | `Function/Render/Spine/CSharp/`（`SpineGpuFormat.cs` / `SpineGpuContainerIo.cs`，netstandard2.1，可整目录拷进 Unity Assets） |

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
   * `influence_count` 个 unorm8 权重的和必须**恰好 255**
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

### 13.2 仍待拍板

**无。** 上表全部已拍板（#2 在 2026-10-22 因实测改判为"逐顶点变长"）；
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
spine.load <骨架 .skel.bytes|.skel|.json> [动画下标]   # 图集取同名 .atlas.txt / .atlas
spine.clear
spine.status
# 多轨（S5，语义 = spine::AnimationState，见 GPU_SPINE_V7_ANIMATION.md §7.0.7）：
spine.play <动画> [track] [loop] / spine.queue <动画> [track] [delay] [loop] / spine.empty [track] [mix]
spine.mix <秒> / spine.speed <倍率> / spine.alpha <track> <alpha>
spine.tintblack <0|1>   # 双色（= SpineSettings::tintBlack），默认 0，跨资产保留
# 启动即加载：ZEditor ... --rhi vulkan --cvar "spine.load G:/.../K_X_Spine.skel.bytes 1"
```

**每帧 CPU 只做两件事**：解算（S5 起是 `SpineGpuAnimationState::Update(dt)` → `Apply(state)` →
`SpineGpuUpdateWorld`，骨骼世界变换 + 槽位状态）与"哪些绘制项可见"
（按 `draw_order`，取槽位当前附件名在 skin 0 下对应的那一项）。**顶点一个都不在 CPU 上算**：

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
  rigid（无 / 有 deform）最大误差 1.2e-4 / 1.2e-4；weighted 5.21 / 5.14 骨架单位。
  weighted 的误差来自**容器里权重是 u8 量化的**（与 deform 无关，两格同量级）；
  把 deform 偏移错一个顶点，这两格会变成 9724 / 402（门槛已接进 verdict）。

**仍然不做**（不是忘了）：多实例；非 default 皮肤；WebGL2（没有 SSBO）。（tint black、DX12 / Metal 已接，见下面。）

#### v15：运行时裁剪（2026-10-23）

3.8 的裁剪只有 CPU 实现（`clipTriangles` 产生新三角形，VS 做不了）。每帧：

1. `SpineGpuClipFrame()` 按**当前**绘制顺序跑 `MeshGeneration/SpineMesh.h:107-259` 的状态机：
   NULL 附件 / 不可画附件 ⇒ `clipEnd(slot)`；裁剪附件 ⇒ `clipStart` 后**直接 continue**（不收尾）；
   可画项 ⇒ 在作用域内就裁，然后 `clipEnd(slot)`；末尾无条件 `clipEnd()`。嵌套被忽略。
2. 作用域内的项在 CPU 上蒙皮（与 `spine_gpu.vert` 同公式、同 u8 权重），裁剪多边形同样蒙皮
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
| 可画槽位"在不在作用域、被哪条裁" | 593 030 次，**0 分叉** |
| 同输入 `clipTriangles`（喂参考的世界坐标 / UV / 三角形 / 多边形） | 333 640 次，输出 2110 万顶点，**逐位相同** |
| 端到端，全 rigid（179 104 项） | 世界坐标最大误差 3.7e-4，UV 3.6e-7 |
| 端到端，含 weighted | 拓扑相同 99.84%；最大误差 35.5 骨架单位 / UV 0.151 —— u8 权重的 5 单位底噪在**近乎平行的裁剪边**上被放大 |

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
* Metal：**没编译、没跑过**（这台机器没有 Apple 工具链）。只做了静态核对：SPIR-V 用 `spirv-cross --msl
  --msl-version 20100 --msl-decoration-binding` 按 MetalRHI 的选项能生成 MSL，`gl_InstanceIndex → [[instance_id]]`，
  9 个 buffer 在 `[[buffer(0..8)]]`（运行时 MetalRHI 再加 8 的偏移）。
* 画面没有人眼看过：屏幕截图（GDI BitBlt）在这台机器上得到全黑（桌面不可见）。

可见集解析实测：OSGame 全量第 0 个动画 t=0 时挂着附件的槽位 29 785 个能找到绘制项，
689 个找不到 —— 这 689 个槽位在 skin 0 下**一个可画项都没有**（"槽位里有别的可画项、
却没有这个名字"的是 **0** 个），即挂的是裁剪 / point / bounding box / path 这类不画的附件。
（`SpineGpuRefParityTest --uv-scan` 输出这三个数。）

---

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
