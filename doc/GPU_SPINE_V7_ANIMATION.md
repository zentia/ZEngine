# GPU-Spine v7：骨骼树 + 动画烘焙，解算自写

- **结论等级**：**设计（未实现）**。边界由 OSGame 实测资产反推（不是拍脑袋），字段已定、出处已核对；代码一行未写。
- **决策**：A2（2026-10-22 拍板）—— *骨骼树 + 动画烘焙进容器 → v7；解算器自己写，仍在 CPU*。
  与规格 §0 一致：解算永远不上 GPU（`AnimationState::update` + `Skeleton::updateWorldTransform`
  是有序串行遍历 + 迭代求解器）。
- **边界来源**：`G:/OSGame/Client_proj/Project/Assets` 下 **782 个 `.skel.bytes`**，
  全部 **3.7.83**，**782/782 解析到精确 EOF、0 失败**。扫描器与结果见 §1。

---

## 1. 边界是怎么定出来的（不靠猜）

扫描器：`G:/OSGame/doc/scripts/scan_spine_skel_timelines.py`
（按 spine-runtimes **3.7** 的 `spine-csharp/src/SkeletonBinary.cs`（910 行）逐行写的字节级解析器，
脚本头注释里列了每条布局依据的行号）。

自检方式：**每个文件解析完必须恰好落在 EOF**。多字节 = 布局理解错，少字节 = 提前 EOF，
两种都进失败清单，绝不"尽力而为"算下去 —— 静默错解析会让后面所有统计变成假的。
实测 **782/782 通过**，这是"3.7 布局转录正确"的强证据。

### 1.1 必须烘焙的时间轴（11 类）

| 类型 | 出现文件数 | 条数 | 帧数 | 载荷 float |
|---|---|---|---|---|
| rotate | 768 | 53 757 | 551 025 | — |
| translate | 774 | 27 314 | 250 035 | — |
| scale | 656 | 9 446 | 83 453 | — |
| shear | 101 | 1 046 | 12 134 | — |
| **attachment** | 630 | 40 010 | 443 778 | — |
| **color** | 650 | 22 620 | 150 520 | — |
| twoColor | 3 | 88 | 88 | — |
| **deform** | **729** | **100 306** | **1 733 134** | **28 316 447** |
| drawOrder | 96 | 174 | 412 | — |
| pathConstraintPosition | 86 | 367 | 1 885 | — |
| pathConstraintMix | 3 | 5 | 33 | — |

### 1.2 完全没用到的（0 文件）—— 可以不实现，但**不要顺手删设计**

`event` / `ikConstraint` 时间轴 / `transformConstraint` 时间轴 / `pathConstraintSpacing`。
（工程里 **events = 0**，所以事件整块在本工程没有需求。）

### 1.3 ⚠️ 最容易被漏掉的边界：约束**本身**必须解算

时间轴是 0，但约束在骨架里存在，而且**每帧都跑**，跟有没有时间轴无关
（`Skeleton.cpp:180` 起 `sortBone` / updateCache 构造；`Skeleton.cpp:199` 起逐项 `update()`）：

| 约束 | 条数 | 涉及文件 | 占比 |
|---|---|---|---|
| IK | 348 | 176 | **22.5%** |
| TransformConstraint | 2 083 | 378 | **48.3%** |
| PathConstraint | 333 | 87 | 11.1% |

⇒ 只做"四种骨骼时间轴"会让 **48% 的资产姿势直接错**，而且错得很安静（约束没跑而已）。
**约束 setup 数据 + 执行顺序必须进容器**，这是 v7 里比时间轴更早要定的一块。

### 1.4 其它体检

| 项 | 值 | 含义 |
|---|---|---|
| 版本 | 全部 3.7.83 | **导入器现在是硬拒 3.8 以外的版本 ⇒ 现在 0/782 能导入** |
| 动画 / 骨骼 / 槽位 | 1 933 / 32 113 / 56 760 | |
| 单文件极值 | 467 骨骼 / 825 槽位 / 6 动画 | 骨骼表 24 B ⇒ 单实例最坏 11 KB，可接受 |
| **多皮肤文件** | **0** | 皮肤维度本工程完全没用上（全是 default） |
| linked mesh | 10 715 条（82 文件） | 既有 `ExactBlobPool` 逐字节去重正好对口 |
| clipping | 46 条（39 文件） | v6 已支持 |
| 双色槽位 | 96（5 文件） | 需要第二套颜色，规模很小 |
| **每帧渲染顶点数** | 均值 1 016.5 / 中位 769 / P90 2 083 / 最大 5 286 | 决定 CPU 动态 VB 的规模（24 B/顶点），见 §5.3 |
| **每顶点影响数** | 最大 **15**，均值 2.20，>4 的占 4.1% | ✅ **已按方案 C 解决**（v7：逐顶点变长，见 §1.5） |

### 1.5 ⚠️ 实测出来的一条硬边界：MAX_INFLUENCES=4 不够（**已解决：C**）

扫描器量的每顶点影响数分布（594 372 个 weighted 顶点）：

| 影响数 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 顶点数 | 159 533 | 267 030 | 107 348 | 36 003 | 13 971 | 5 177 | 2 276 | 1 003 | 725 | 470 | 347 | 294 | 121 | 53 | 21 |
| 占比 | 26.8% | 44.9% | 18.1% | 6.1% | 2.4% | 0.9% | 0.4% | 0.2% | 0.1% | … | | | | | |

**> 4 的有 24 458 个顶点（4.1%），最大 15。** 导入器按拍板的规则「n > 4 报错并报出附件名」
⇒ **实测 297 次 bake 失败**。这是 S0 跑完 782 个资产后**唯一剩下的阻塞项**
（另有 1 个是资产本身缺图集 region，属于正确报错）。

三个候选解法（都要动容器布局 ⇒ 落在 v7 里）：

| | 做法 | 内存（weighted） | 备注 |
|---|---|---|---|
| A | `MAX_INFLUENCES` 提到 16 | 固定 **176 B/顶点**（原 48 B 的 3.7×） | 最省事，但**与"降内存"的目标相反** |
| B | 容器记 `max_influences`，步长随资产变 | 该资产的最大值决定，多数资产仍是 48 B | header 已 64 B 占满 ⇒ 需要新 chunk 或 v7 重排 |
| C | **逐顶点变长**：`(offset, count)` + 影响流 | 均值 ≈ **34 B/顶点**（原 48 B 的 0.72×） | 省内存最多、ALU 也少（循环 2.2 次而不是 4 次），但要改 shader 的取数方式 |

**已拍板 C（2026-10-22）并落地**：它同时是"降内存"和"降运算"的那一条，
正好对上本次的目标；代价是顶点取数从 `base + v * stride` 变成"按 offset 取"，
以及 shader 里改成 `[loop]` 的动态循环。

落地形态（容器 **v7**）：

* weighted 顶点 48 B → **8 B**：只剩 `(influence_offset u32, influence_count u32)`；
* 蒙皮数据进 **chunk 21**（`bindPos f32×2`，8 B/条）与 **chunk 22**
  （`boneIdx u16 + weight unorm8 + pad`，4 B/条）。两条流**共用同一个条目下标**；
* 两条流**分开**而不是交错成一条 12 B —— `Load2` 要 8 B 对齐，12 B 步长会让
  第 1、3、5…条的 bindPos 落在 12、36…（不是 8 的倍数）；
* `MAX_INFLUENCES` 保留为 **255**，只当"数据坏了"的硬上限（实测最大 15，正常资产碰不到）；
* header **一个字节都没加**：影响条目数从 chunk 表推导（`size / stride`）；
* 导入顺序有讲究（写错不报错、形状静默错）：
  **影响流按"逐字节相同"去重 → 用去重后的块起点改写顶点块里的本地下标 → 顶点块去重。**
  先改写顶点块的话，两个相同附件的本地下标都是 0 ⇒ 顶点去重看着命中了，
  但它们指的其实是各自那段影响流。

**782 个资产重跑结果：781 成功 / 1 失败**（此前 484 / 298）。
剩下 1 个是资产本身的数据错误，见 §7.0。

**实测省了多少**（`doc/scripts/measure_zspg_chunks.py`，781 个容器全量统计）：

| | v6（定长 4 槽 48 B，假设能全部烘焙） | v7（实测） | 比值 |
|---|---|---|---|
| 顶点 + 蒙皮 | 27.09 MB | **19.54 MB** | **0.721** |
| 再加上 UV + 索引 | 39.04 MB | **31.48 MB** | 0.806 |
| weighted 顶点数 / 影响条目 | 569 627 | 569 627 / **1 238 604** | 均值 **2.174** 个影响 |

⇒ 蒙皮相关内存是原来的 **72%**，shader 循环 **2.174 次**而不是固定 4 次（ALU ≈ 54%）。
两个数字都是量出来的，不是按均值算出来的 —— 顶点块与影响流都做过逐字节去重，
所以实测比"569 627 × (8 + 12 × 2.174)"的理论值还要更小一点。

---

## 2. v7 改什么 / 不改什么

| | v6 | v7 |
|---|---|---|
| `SpineGpuHeader`（64 B，已占满） | 有 | **不动** |
| chunk 1..9 | 有 | **不动**（含落盘顺序；v7 后整体顺序是 1,2,3,**21,22**,4,5,9,6,7,8） |
| §5 每帧实例数据（骨骼表 24 B / 实例项 20 B / root 24 B） | 有 | **不动** |
| §6 shader 数学 | 有 | **不动** |
| weighted 顶点 | 48 B（定长 4 槽） | **8 B 引用** + 影响流 chunk **21 / 22**（§1.5 方案 C，已落地） |
| 新增 | — | chunk **10..20**（见 §3） |
| `kFormatVersion` | 6 | **7** |
| 导入器版本策略 | 只认 3.8 | **3.7 与 3.8 同一条解析路径**（见 §4） |

**header 一个字节都不加**：新增计数一律从 chunk 表推导（`size / stride`）。
header 大小一旦随版本变，C# 读端就得"按 version 分支读 header"，那是同一事实的第二份表述。

---

## 3. 新增 chunk

| id | 内容 | 单元 |
|---|---|---|
| 10 | 骨骼 setup（树 + 局部变换） | 36 B/骨 |
| 11 | 动画目录 | 20 B/条 |
| 12 | 时间轴表（全局一张，动画切片引用） | **32 B**/条 |
| 13 | 帧数据（float 平面数组） | 4 B/float |
| 14 | 曲线类型（u8[]，每段一个） | 1 B/段 |
| 15 | bezier 控制点（float[4]，按段序） | 16 B/段 |
| 16 | 时间轴变长载荷（deform / drawOrder / 附件名） | 字节 |
| 17 | 槽位 setup（颜色 + 暗色） | 32 B/槽 |
| 18 | 约束 setup（IK / transform / path 三段） | 见 §3.6 |
| 19 | update 顺序（(kind, index) 序列） | 4 B/项 |
| 20 | PathAttachment（lengths / closed / constantSpeed） | 见 §3.7 |

### 3.1 chunk 10 — 骨骼 setup（36 B/骨）

```
float    x, y, rotation, scale_x, scale_y, shear_x, shear_y;  // @0 .. @27
float    length;                // @28   IK / 编辑器挂点
int16_t  parent;                // @32   -1 = 根
uint8_t  transform_mode;        // @34
uint8_t  pad;                   // @35
```
字段集合抄 `BoneData.h:110-115`；`transform_mode` 取值抄 `TransformMode.h:34-40`。
**顺序不变量 `parent < index`**，违反即 Bake 报错，绝不静默重排 —— 重排会让"骨骼下标"
在容器里有两种含义。有了它，正向遍历就是拓扑序，`updateWorldTransform` 是单趟 for。

### 3.2 chunk 12 — 时间轴表（32 B/条）

```
uint16_t type;            // @0   TimelineType 原值（TimelineType.h:35-49），不自造枚举
uint16_t target;          // @2   骨骼 / 槽位 / ik / transform / path 下标
uint32_t frame_count;     // @4
uint32_t frame_first;     // @8    进 chunk 13 的**帧**下标（不是 float 下标）
uint32_t curve_first;     // @12   进 chunk 14 的**段**下标
uint32_t bezier_first;    // @16   进 chunk 15 的**贝塞尔序号**下标
uint32_t bezier_count;    // @20
uint32_t payload_offset;  // @24   进 chunk 16 的**字节**偏移
uint32_t payload_count;   // @28   条目数
```
`frame_first` 单位是"帧"不是"float"，与 `uv_offset` 用"条目"是同一约定
（单位写错时 `0 × stride` 与 `0` 逐字节相同，只有**非 0** 的断言能抓到）。
⚠️ 夹具必须造一条 `frame_first` / `curve_first` **非 0** 的时间轴。

### 3.3 chunk 14/15 — 曲线**（原设计的定长 20 B/段已否决）**

实测 **2 567 358 段**：linear 58.2% / stepped 5.2% / bezier 36.6%。

| 方案 | 总大小 |
|---|---|
| 定长 `SpineGpuCurveSegment` 20 B/段（原设计） | **49.0 MB** |
| 紧凑：chunk 14 一个 u8/段 + chunk 15 每条 bezier 4 个 float | **16.8 MB** |

差 32 MB（65%）⇒ **采用紧凑**。原设计担心的"段 → bezier 序号要前缀和"被
`bezier_first` + `bezier_count` 两个显式字段消掉了（运行时不扫、不推）。

- 段数 = `frame_count - 1`；段 i = 帧 i → 帧 i+1。
- type 常量抄 `CurveTimeline.cpp:42-44`（LINEAR=0 / STEPPED=1 / BEZIER=2）。
- 存**控制点**不存采样表（4 float vs 18 float）。展开式常数
  `0.03f / 0.006f / 0.3f / 0.16666667f`（`CurveTimeline.cpp:78-81`）**照抄**，化简即错且不报错。

### 3.4 chunk 16 — 变长载荷

- **attachment（type 4）**：每帧一个 `u32` = 动画附件名在 chunk 1 里的偏移；
  `0xFFFFFFFF` = 空名（隐藏该槽位）。
- **deform（type 6）**：每帧 `[u32 start][u32 count][float × count]`
  （`count == 0` = 回退帧，抄 `SkeletonBinary` 的 `end == 0` 语义）。
  ⚠️ **保持稀疏，不要在 Bake 时展开成整帧** —— 见 §5。
- **drawOrder（type 8）**：每帧 `[u32 offset_count][(u32 slot, u32 offset) × offset_count]`，
  未列出的槽位按 setup 顺序填（抄 `SkeletonBinary` 的 unchanged 收集规则）。

### 3.5 chunk 17 — 槽位 setup（32 B/槽）

`color rgba` f32×4 @0 + `dark rgba` f32×4 @16。
color / twoColor 时间轴是**相对 setup 颜色**混合的，容器里没有 setup 颜色就没法算。
（v6 只有一个 `uses_tint_black` 布尔，没有颜色值 —— 这是一个既有缺口，不是新增需求。）

### 3.6 chunk 18 — 约束 setup

字段集合抄 `SkeletonBinary` 3.7：

* **IK**（`:204-215`）：`order` / `bones[]`（长度可变）/ `target` / `mix` /
  `bendDirection`(sbyte) / `compress` / `stretch` / `uniform`。
* **TransformConstraint**（`:217-237`）：`order` / `bones[]` / `target` / `local` / `relative` /
  `offsetRotation` / `offsetX` / `offsetY` / `offsetScaleX` / `offsetScaleY` / `offsetShearY` /
  `rotateMix` / `translateMix` / `scaleMix` / `shearMix`。
* **PathConstraint**（`:239-257`）：`order` / `bones[]` / `target`（**槽位**不是骨骼）/ `positionMode` /
  `spacingMode` / `rotateMode` / `offsetRotation` / `position` / `spacing` / `rotateMix` / `translateMix`。
  ⚠️ `position` / `spacing` 是否乘 scale 取决于 `positionMode == Fixed` /
  `spacingMode ∈ {Length, Fixed}`（`:249-251`）—— 这是**解析期**的事，别拖到运行时。

`bones[]` 长度可变 ⇒ chunk 内放一段 `uint16` 索引池，每条约束存 `bone_count` + `bone_first`。
**每条结构的字节大小在实现时用 `static_assert` 钉死**（本设计只钉字段集合，避免纸上算错偏移）。

### 3.7 chunk 19/20 — 执行顺序与 PathAttachment

* **chunk 19**：`Skeleton::updateWorldTransform` 遍历的 `_updateCache` 顺序
  （`Skeleton.cpp:180` 起的 `sortBone` + 约束 `order` 排序结果），
  落盘成 `(kind u8: 0=bone/1=ik/2=transform/3=path, index u16)` 序列。
  **直接落结果，不落"让运行时重排"** —— 重排规则有 `sortBone` 的递归 +
  约束 `order` 两层，运行时再实现一遍就是给自己多一次写反的机会。
* **chunk 20**：PathConstraint 的 target 是槽位上的 **PathAttachment**，需要
  `closed` / `constantSpeed` / `lengths[]`（`SkeletonBinary` 3.7 `:455-463`，
  `lengths` 长度 = `vertexCount / 3`）。

---

## 4. 3.7 与 3.8：同一条解析路径，一个版本分支

实测：把 `spine-runtimes 3.7` 的 C# 与本机 3.8 C++
（`G:/OSGame/unity-engine-source/Modules/Spine/spine-cpp`）**逐段比对**，
骨骼 / 槽位 / IK / transform / path / 皮肤 / 附件 / 事件 / **动画** 的**字节布局完全一致**。

唯一差异：**槽位暗色的判据**
* 3.7：`int darkColor = ReadInt(); if (darkColor != -1) hasSecondColor = true;`
* 3.8：读 4 个字节 `(r,g,b,a)`，`if (!(r==0xff && g==0xff && b==0xff && a==0xff)) hasDarkColor = true;`

⇒ 导入器**不需要第二套解析器**，只需要：
1. 版本检查从"必须 3.8"放宽到"3.7.x 或 3.8.x"（现在实测 **0/782 能导入**，
   报错文案是 `骨架 ...：skel: spine 版本是 '3.7.83'，本导入器锁定 3.8（规格 §1.1）`）；
2. 暗色判据按版本分支；
3. `spine_runtime` 字段照实写（3.7.83 → `30783`），别再假装是 30800。

⚠️ 参考源是 **Unity 改过的 3.8 分支**（`CurveTimeline` 存储被重排过，
`CurveTimeline.h:70-82` 自述 "Replaces the former BEZIER_SIZE of 19"）。
数学没变（`BEZIER_VALUES` 仍是 18，`CurveTimeline.cpp:45`），布局是我们自己定的 —— 不受影响。
但**转录必须逐行对**，不能照记忆写。

---

## 5. deform：不做稠密展开，做「deform 增量」（本节结论已改写）

**93.2% 的资产（729/782）有 deform**，共 100 306 条时间轴、1 733 134 帧、
**28 316 447 个 float（≈113 MB，占源 .skel 总体积 197 MB 的 57%）**。
deform 稠密长度实测：**均值 30.4 float（122 B）/ 中位 8 / P90 68 / 最大 5 110（20 KB）**。

### 5.1 被否决的写法：稠密展开 / 全量 CPU 动态 VB

* **稠密展开**（把每帧按 `deformLength` 补齐给 GPU 做两帧 lerp）：
  1 733 134 帧 × 百级顶点 × 2 ≈ **3 亿 float，GB 级**，直接不可行。
  （现网的 2 831 万正是因为它**没展开**。）
* **OSGame 引擎那种"全量世界空间 CPU 动态 VB"**（`SpineMesh.h:266` `BuildMeshWithArrays`
  写 `(tempVerts[iii] + x) * s`，来自 `computeWorldVertices`）也不行 ——
  它把**蒙皮**一起搬回 CPU，VB 变成每实例一份，**正好把 §0 的目的（VB 共享）废掉**。
  代价实测：每骨架每帧渲染顶点 **均值 1 016.5 / 中位 769 / P90 2 083 / 最大 5 286**，
  24 B/顶点 ⇒ 每实例每帧重写+上传 **均值 24.4 KB / 最大 126.9 KB**，
  且 `UploadMeshData(false)` 保留 CPU 镜像 ⇒ 常驻 **48 B/顶点（均值 48.8 KB）**。
  它唯一的优化 `GeometryNotEqual`（`SpineMesh.cpp:170-208`）只省三角形，省不掉顶点 ——
  这是 CPU 动态 VB 的结构性天花板。
  完整论证见 **§5.3**。

### 5.2 v7 的决定：deform 是「蒙皮之前的局部空间加数」，所以只需增量

判据 `spine-cpp/src/spine/VertexAttachment.cpp:64-131`（OSGame 引擎 3.8）：

* **rigid**（`bones.size() == 0`，`:71-87`）：deform 数组非空时 `vertices = deformArray`
  （`:72-74`）——**整体替换局部坐标**，再由单骨骼矩阵变换（`:83-84`）。
* **weighted**（`:114-130`）：`vx = (*vertices)[b] + (*deformArray)[f]`（`:122-123`），
  `f` 每**影响**前进 2（`:119` `f += 2`）⇒ 每个影响一个 2 float 偏移，
  **先加到 bind pose 局部坐标，再乘骨骼矩阵、再乘权重**（`:125-126`）。

⇒ **deform 与骨骼矩阵无关**，它只是蒙皮前的一个加数（rigid 是替换）。
所以回退**不需要**重建世界空间 VB：

| | 顶点取值 | 蒙皮 |
|---|---|---|
| rigid | `v = deform`（替换） | GPU，`B · v` |
| weighted | `v = bindLocal + deform`（逐影响相加） | GPU，`Σ wᵢ·Bᵢ·vᵢ` |

**共享的 bind pose VB 完全不动**，每实例每帧只需要上传「当前帧的 deform 数组」：
均值 122 B/条（对比全量 24.4 KB/帧，两个数量级），且只上传**被命中的绘制项**。

于是：

* deform **数据烘焙进容器**（保持稀疏，chunk 16）——不变；
* 运行时**求解**（不是查表），把结果写成 per-instance deform 缓冲；
* **推荐 B**：共享 bind pose + per-instance deform 缓冲，GPU 采样，蒙皮全在 GPU；
* **台阶 C**：只给命中绘制项写"deform 后的局部坐标"的小动态 VB，蒙皮仍在 GPU；
  B 与 C 都不破坏 VB 共享，C 改动小、可先落地；
* **OSGame 式全量世界空间动态 VB（方案 A）：否决**，它不满足 §0 的目的。

⚠️ 语义不对称必须在 shader 里显式分两条路：**rigid 替换 / weighted 相加**。
两种顶点格式（rigid 存 bindPos / weighted 存影响流引用）本来就是两条路径，
不额外增加分支 —— v7 之后两者**步长都是 8 B**，但含义不同，别因为步长一样就合并。
deform 数组长度对 weighted 是 **ΣboneCount × 2**（不是 vertexCount × 2），
容器里要按条记录 `deformLength` 才能分配缓冲。

### 5.3 附：CPU 动态 VB 的代价清单（OSGame 引擎 3.8，逐条有出处）

驱动链 `UnitySkeletonGraphic.cpp`：`Update()`（`:218-236`，solve）→
`LateUpdate()`（`:276-283`）→ `UpdateMesh()`（`:286-313`）
→ `mesh->Clear(false)`（`:296`）→ `BuildMeshWithArrays`（`:304`）
→ `FillVertexData`（`:306`）→ `SetBounds` → `UploadMeshData(false)`（`:324`）。

| # | 缺点 | 出处 / 量化 |
|---|---|---|
| 1 | **VB 不可共享**（每实例一份世界空间 VB）——直接违背 §0 | `SpineMesh.h:266,304,382` + `:405` |
| 2 | 每帧全量重写+上传 24 B/顶点 | `:337-349`（region）/`:412-419`（mesh）；均值 24.4 KB、最大 126.9 KB |
| 3 | CPU 镜像 + GPU **双份**常驻 | `UploadMeshData(false)`；48 B/顶点，均值 48.8 KB |
| 4 | **uv / color 是静态数据却每帧重写**（24 B 里的 12 B 纯浪费） | `:342-349`、`:369-372` |
| 5 | 每实例一个 Mesh ⇒ 无法 instancing / 合批 | `FillVertexData` 写 `PPtr<Mesh>` |
| 6 | CPU 蒙皮成本随实例数线性增长 | `:119-127` weighted 内层循环 |
| 7 | 有裁剪时更糟：三角化每帧重跑 | `SpineMesh.cpp:176-177`，`hasActiveClipping` 时 `GeometryNotEqual` 恒 true |
| 8 | 优化天花板低：只有 `GeometryNotEqual` 一个杠杆，顶点重写省不掉 | `SpineMesh.cpp:170-208` |
| 9 | 分配抖动：`resize` / `ResizeVertices` / `tempVerts.setFixedSize` 增长 | `SpineMesh.h:271-275,380-381` |

公平地说：`Update()` 里的 solve（`:232-235`）在**两种设计里都要付**，不是 CPU 动态 VB 的额外成本；
额外成本全在 `LateUpdate()` 的建 mesh 与上传。另外该模块**没有任何 GPU 路径**
（引擎内无 Spine 着色器、无 ComputeShader / StructuredBuffer）⇒ 它是纯 CPU 实现，
可以作为"我们要避免的形状"来读，不能当作"我们要抄的方案"。

---

## 6. 时间轴语义：三条错了不报错的（核对时挖出来的）

1. **Scale 是乘、Translate/Shear 是加**。判据 `ScaleTimeline.cpp:75-76`
   `x = _frames[... + PREV_X] * bone._data._scaleX`，混合分支带 `abs`/`sign`（负缩放翻面）。
2. **Rotate 走最短弧**，靠 `(16384 - (int)(16384.499999999996 - r/360))*360`
   （`RotateTimeline.cpp:100-101`），别"顺手"改 fmod。
3. **`binarySearch` 返回下一帧首下标**（`Animation.cpp:84-106`）⇒ 前一帧用**负**偏移
   `PREV_TIME=-3 / PREV_X=-2 / PREV_Y=-1`（`TranslateTimeline.cpp:49-51`；rotate 是 -2/-1）。

另有两条来自实测的：
4. **约束的 `uniform` / `local` / `relative` 等标志全在 setup 里**，
   时间轴只带 mix 之类的标量 —— 漏掉 setup 就永远算不对（§1.3）。
5. **多轨混合里同一条动画不同时间轴的 blend 可能不同**：
   `AnimationState.cpp:425` `timelineMode[ii] == Subsequent ? blend : MixBlend_Setup`，
   而 `timelineMode` 由 `setTimelineModes`（`:978-1023`）按
   `getPropertyId() = (TimelineType << 24) + target` 与更高轨的命中情况逐条决定。
   把整条动画统一成一个 blend 是错的，且看不出来。

---

## 7. 分阶段实施

### 7.0 S0 已落地（2026-10-22）与其实测结果

| 项 | 落地 | 结果 |
|---|---|---|
| 版本闸门 | `ClassifySpineVersion()`：只认 3.7.x / 3.8.x，4.x 明确拒绝 | 782 个 3.7.83 资产**全部**进得来（此前 0） |
| 暗色判据 | 按版本分支：3.7 `darkColor != -1`（`SkeletonBinary.cs:190-192`）；3.8 四分量不全为 ff（`SkeletonBinary.cpp:186-189`） | 实测 96 个双色槽位判对 |
| `spine_runtime` | **写实** 30700 / 30800，不再一律写 30800 | 读端能据此选判据 |
| Unity 命名 | `<名>.skel.bytes` / `<名>.atlas.txt`；输出 `<名>.zspg`（不是 `<名>.skel.zspg`） | 否则 782 个一个都跑不了 |
| deform → 无绘制项附件 | path / boundingbox / point / clipping 记进 `no_draw_attachments`，deform 指向它们 ⇒ **跳过**且不点亮 `kHasDeform` | 实测 88 处 path + 4 处 clipping；修回 **68 个资产** |

**跑完 782 个的实测（S0 当时）**：**484 成功 / 298 失败**，失败里
**297 = MAX_INFLUENCES > 4**（§1.5）、
**1 = 资产自己缺图集 region**（正确报错，不动）。

### 7.0.1 S0.5 已落地（2026-10-22）：逐顶点变长影响（方案 C）

| 项 | 落地 |
|---|---|
| 布局 | weighted 顶点 48 B → **8 B 引用**；新增 **chunk 21**（bindPos 8 B/条）/ **22**（bone+weight 4 B/条） |
| `MAX_INFLUENCES` | 4 → **255**（只当"数据坏了"的硬上限） |
| 去重顺序 | 影响流先去重 → 改写顶点块本地下标 → 顶点块去重（顺序错了不报错、形状静默错） |
| 读端校验 | 两条流条目数必须相等；每个权重顶点的 `[offset, offset+count)` 必须落在流里 |
| GPU 侧 | `SpineSkinningParityTest` 的 shader 改成 `[loop]` 动态循环，影响数覆盖 1..6 |

**782 个资产重跑**：**781 成功 / 1 失败**（297 个 MAX_INFLUENCES 失败全部消失）。
剩下的 1 个 = 上面那个"资产自己缺图集 region" —— 已查证为真的数据错误：
`CustomResources/UGUI/Spine/Zhanling/Jing_wuxianzhuilie/UI_Jing_Spine.atlas.txt:42`
的 region 名是 **`guizi `**（尾部多一个空格），而骨架按 `guizi` 查。
参考实现的 `Atlas::trim()`（`Atlas.cpp:240-256`）只去**行首**空白和尾部 `\r`，
不去尾部空格 ⇒ spine-unity 同样找不到 ⇒ **报错是对的，不是我们的 bug**。

> ⚠️ 批量跑的时候另外还有 28 条 `[FAIL] 找不到同名图集`，那是**噪声不是失败**：
> 套件把目录里所有 `.json` 都当骨架试了一遍，其中 25 个是工程配置 json、
> 3 个是 Shader Forge 材质 json（如 `Art_Resources/Characters/Hero/193_LaFeiEr/Textures/2.json`）。

`SpineGpuImportTest` 897 → **933/933**；C# 跨语言对拍 193 → **219/219**；
`SpineSkinningParityTest` **11/11 顶点、最大 0.68 ulp**（与改前同）。

| 阶段 | 内容 | 验收 |
|---|---|---|
| **S0** | 导入器放宽到 3.7 + 逐顶点变长影响（方案 C） | **已完成**，781/782（见 7.0 / 7.0.1） |
| **S1** | chunk 10 骨骼树；C++ 写/读 + C# 读 + 字节 round-trip | 语义断言（逐字段），不只比大小 |
| **S2** | 动画解析（json + 二进制）+ chunk 11-16；11 类时间轴全覆盖 | 夹具：单帧 / 多帧 / linear / stepped / bezier / 负缩放 / 超 180° 旋转 / 稀疏 deform / 非 0 偏移 |
| **S3** | 槽位颜色（chunk 17）+ color / twoColor / attachment / drawOrder 解算 | 对拍 |
| **S4** | 约束（chunk 18-20）+ update 顺序 | 对拍：376 个有约束的资产子集，逐骨骼比世界变换 |
| **S5** | 多轨混合（`AnimationState` 转录，含 `_timelinesRotation` 最短弧记忆） | 对拍：随机切轨 / 混出，逐帧比对 |
| **S6a** | deform 求解 + per-instance deform 缓冲（方案 C：只给命中项写"deform 后局部坐标"，蒙皮仍在 GPU） | 与 `SpineSkinningParityTest` 接上；断言共享 VB **没被改写** |
| **S6b** | 方案 B：deform 缓冲直接由 GPU 采样，shader 里 rigid 替换 / weighted 相加 | 同上，且逐实例上传量 ≤ 122 B × 命中条数 |

**对拍夹具是硬要求**（照 `SpineSkinningParityTest` 9/9、0.68 ulp 的先例）。
动画解算没有对拍的话，"看起来在动"会掩盖所有语义错误。

---

## 8. 风险 / 未决

1. ~~**deform 的取舍需拍板**~~ —— **已定**：稠密展开否决、OSGame 式全量 CPU 动态 VB 否决，
   走「共享 bind pose + deform 增量」（§5.2）。剩下的只是 B/C 谁先落地，**不改变 v7 体量**。
2. **约束解算的参考源**：本机 3.8 C++ 有 `IkConstraint.cpp` / `TransformConstraint.cpp` /
   `PathConstraint.cpp`，但本工程资产是 **3.7**。3.7/3.8 的约束**数学**是否一致尚未逐行核对
   （只核对了**字节布局**）。S4 开工前必须补这一步。
3. **双色（tint black）**：96 槽位 / 5 文件，规模很小，但要在渲染侧多一路颜色。
   若嫌麻烦可先 bake 报错，但那样这 5 个资产就出不来。
4. **多皮肤 = 0**：v2 的皮肤维度在本工程没用上。不删（换装是容器承诺的能力），
   但也说明皮肤相关的测试覆盖对 OSGame 没价值。
5. **容器体量**：按 §3.3 的紧凑曲线 + 稀疏 deform 粗估，容器 ≈ 源 .skel 的 **75%~80%**
   （源 197 MB）。如果这不可接受，下一个可压的点是帧数据 fp32 → 定点，
   但那会引入精度分歧，**先不动**。
