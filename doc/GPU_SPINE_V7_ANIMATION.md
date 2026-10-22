# GPU-Spine v7：骨骼树 + 动画烘焙，解算自写

- **结论等级**：**部分已落地（S0 / S0.5 / S1 / S2 / S3 / S4a），S4b 及以后仍在设计阶段**。
  边界由 OSGame 实测资产反推（不是拍脑袋），字段已定、出处已核对。
  已落地：影响流（v7）、骨骼树（v8）、动画（v9）、槽位 setup（v10）、
  **约束 setup / 执行顺序 / PathAttachment（v11）**，见 §7.0 / 7.0.1 / 7.0.2 / 7.0.3 /
  7.0.4 / **7.0.5**。未落地：**约束解算（S4b，解算器已写出、706/781 对拍通过，见 §7.0.6）**、
  deform（S6）、多轨混合（S5）、GPU 蒙皮上线路。
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

✅ **已落地（v11，2026-10-23）**：chunk 18（三类约束 setup + 共用骨骼池）、
chunk 19（`updateCache()` 的执行顺序与复位表）、chunk 20（PathAttachment 几何），
见 **§7.0.5**。上表的条数是从源 `.skel` 扫出来的（IK 348 / transform 2083 / path 333），
容器里实测 **344 / 2082 / 333** —— transform 的 1 条差来自资产侧的解析噪音，
量级一致。**⚠️ 落盘 ≠ 解算**：解算本身仍是 S4b。

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

### 3.1 chunk 10 — 骨骼 setup（36 B/骨）—— **已落地，见 §7.0.2**

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
uint16_t target;          // @2   骨骼 / 槽位 / **附件记录** / 约束 下标（见下）
uint32_t frame_count;     // @4
uint32_t frame_first;     // @8    进 chunk 13 的 **float** 下标（不是"帧"下标！）
uint32_t curve_first;     // @12   进 chunk 14 的**段**下标
uint32_t bezier_first;    // @16   进 chunk 15 的**条**下标
uint32_t bezier_count;    // @20
uint32_t payload_offset;  // @24   进 chunk 16 的**字节**偏移
uint32_t payload_count;   // @28   条目数（变长三类是帧数，其余恒 0）
```

⚠️ **`frame_first` 的单位是 float 下标，不是"第几帧"** —— 原设计写的"帧下标"是错的：
各类时间轴**每帧的 float 数不同**（rotate 2 / translate 3 / color 5 / twoColor 8 …），
压根不存在一个全局统一的"帧"单位。用"帧"去算下标，第一条（偏移 0）看不出错，
第二条起就读到别人的帧数据，且不报错。

`target` 的含义随 `type` 变，这也是它叫 target 不叫 bone_index 的原因：

| type | target |
|---|---|
| rotate / translate / scale / shear | **骨骼**下标 |
| attachment / color / twoColor | **槽位**下标 |
| deform | **附件记录**（chunk 7 行号）下标 |
| ik / transform / path* | 对应**约束**下标 |
| drawOrder | 恒 0（它作用于整个骨架的槽位顺序） |

deform 用"附件记录下标"是有意的：一条记录 = (皮肤, 槽位, 附件) 三元组，正好是 deform
时间轴的寻址键，不需要再给 32 B 塞三个字段。代价是"deform 指向一个不产生绘制项的附件"
（path / boundingbox / clipping）无从表达 —— 但它们**本来就不渲染**，Bake 期直接跳过。

⚠️ 夹具必须造一条 `frame_first` / `curve_first` **非 0** 的时间轴（上面的单位陷阱只有
非 0 断言能抓到）。

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

### 3.5 chunk 17 — 槽位 setup（**40 B/槽**，已落地，见 §7.0.4）

| 偏移 | 字段 | 说明 |
|---|---|---|
| @0..@12 | `color rgba` f32×4 | 默认 **(1,1,1,1)**（`SlotData.cpp:39`）。color / twoColor 时间轴是**相对 setup 颜色**的，容器里没有 setup 颜色就没法算 |
| @16..@28 | `dark rgba` f32×4 | 默认 **(0,0,0,0)**（`SlotData.cpp:40`） |
| @32 | `setup_attachment_name_offset` u32 | 字符串表**第 4 组**的字节偏移；`kNoAttachmentName = 0xFFFFFFFF` = 空名 |
| @36 | `flags` u32 | 目前只有 bit0 = `has_dark` |

> 原设计写的是 32 B/槽。实现时**不够**：还要一格放 setup 附件名偏移（attachment 时间轴的
> 复位基线，`AttachmentTimeline.cpp:65-68`）和一格放逐槽位 flags。⇒ **40 B = 5 × 8**，仍是 8 B 对齐。

三条**反直觉**的（每条都能造成"能跑、不报错、结果错"）：

1. **dark 的 alpha 恒为 1**（只要这个槽位有暗色）—— 参考实现写死的：
   二进制 `SkeletonBinary.cpp:188` `set(r/255, g/255, b/255, 1)`；
   json `SkeletonJson.cpp:230` `darkColor.a = 1`。
   二进制里读进来的第 4 个字节只用来判"有没有暗色"，**从不作为 alpha 落地**。
   json 的 `"dark"` 是 **6 位**十六进制（只有 rgb），而 `"color"` 是 **8 位**（rgba）。
2. **`has_dark` 不能由 dark 是否全 0 推断** ⇒ 必须有显式 flag：
   默认 dark 就是 (0,0,0,0)（`SlotData.cpp:40`），而 3.7 的判据是
   `读到的 int32 != -1`（`SkeletonBinary.cs:190-192`）⇒ **0x00000000（纯黑）照样是"有暗色"**。
3. **`SlotData::_attachmentName` 是 attachment 时间轴的复位基线**（`AttachmentTimeline.cpp:65-68`）：
   `time < frames[0]` 时槽位**复位到 setup 附件**，不是"隐藏"。
   ⇒ 它必须落盘，否则只能复位成"空"（`time < frames[0]` 这段的附件全画错）。

（v6 只有一个 `uses_tint_black` 布尔，没有颜色值 —— 这是一个既有缺口，不是新增需求。）

### 3.6 chunk 18 — 约束 setup（**已落地，见 §7.0.5**）

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

### 3.7 chunk 19/20 — 执行顺序与 PathAttachment（**已落地，见 §7.0.5**）

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

### 4.1 ⚠️ 但**约束数学 3.7 与 3.8 并不一致**（S4 前置，2026-10-22 已核对完）

上一段只比了**字节布局**。S4 要解算约束，所以把**代码体**也逐行比了一遍：

```
A = /i/spine-runtimes/spine-cpp/spine-cpp/src/spine      # 3.7.83（与资产同版本）
B = G:/OSGame/unity-engine-source/Modules/Spine/spine-cpp/src/spine   # 3.8
```
（比之前先 `tr -d '\r'` + 折叠空白 + 剥掉文件头的 license 注释 —— 不剥的话
10 个文件每个都有 500+ 行的"差异"，全是版权声明换版本，真正的差异会被淹掉。）

**结论：10 个文件里只有 2 处语义差异，两处都是 3.8 修掉的 3.7 bug。**

| # | 位置 | 3.7 | 3.8 | 判据 |
|---|---|---|---|---|
| 1 | `Bone::updateWorldTransform()` **根骨骼分支**<br>`Bone.cpp:110-113` / `:109-112` | `_b = cosDeg(rotationY) * scaleY * **sy**`<br>`_c = sinDeg(rot+shearX) * scaleX * **sx**` | `_b = … * **sx**`<br>`_c = … * **sy**` | **3.8 对**：世界矩阵 = `diag(sx,sy) · L`，第 2 列（b,d）乘 `sx`、第 2 行（c,d）乘 `sy`。3.7 用反了轴 |
| 2 | `IkConstraint::apply()` **单骨骼**分支<br>`IkConstraint.cpp:63` / `:62` | `if ((compress && dd < b) \|\| (stretch && dd > b) && b > 0.0001f)` | `if (((compress && dd < b) \|\| (stretch && dd > b)) && (b > 0.0001f))` | **3.8 对**：C++ 里 `&&` 比 `\|\|` 紧，3.7 实际解析成 `(compress && dd<b) \|\| ((stretch && dd>b) && b>0.0001f)` —— `b ≈ 0` 且开 `compress` 时会算出 `dd/b = 0/0 = NaN`，整条骨骼矩阵变 NaN |

**逐文件核对结果**（除上表外全是噪声级差异）：

| 文件 | 结论 |
|---|---|
| `TransformConstraint.cpp` | **完全一致**（只有 `RTTI_IMPL` 多一个参数） |
| `PathConstraint.cpp` | **完全一致**（只有 `Vector::setSize` → `setFixedSize` 的 API 改名） |
| `Skeleton.cpp` | `updateWorldTransform` / `sortBone` **完全一致**；差异只有 `setSkin` 返回 bool、`PathAttachment::_bones` 元素类型 `size_t`→`unsigned short`、`setFixedSize` |
| `Constraint.cpp` / `*ConstraintData.cpp` / `BoneData.cpp` | **完全一致** |

⇒ **S4 的决定：实现 3.8 的那两处（修好的）数学**，不是照抄 3.7。理由：

1. 两处都是 bug fix，不是行为变更 —— 抄 3.7 等于主动把 NaN 搬进来。
2. 第 1 处对我们**是休眠的**：`Skeleton::_scaleX/_scaleY` **不从资产文件里读**
   （`SkeletonBinary.cpp:156-157` 那个 `data->_scaleX` 是 **BoneData** 的不是 Skeleton 的），
   只有应用主动调 `setScaleX/Y` 才会变，默认 (1,1) ⇒ `sx == sy` ⇒ 两版算出来一样。
3. 第 2 处只在 `b = bone.length * scaleX ≈ 0` 且开 `compress` 的退化输入上分叉，
   3.7 在那里给 NaN ⇒ 美术不可能"照着 NaN 调过动画"。

（⚠️ 这条推翻了 §8 第 2 项原先的措辞 —— 它写的是"是否一致尚未核对"。现在核对完了：
**不一致，两处，都按 3.8 实现**。）

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
| **S1** | chunk 10 骨骼树；C++ 写/读 + C# 读 + 字节 round-trip | **已完成**（见 7.0.2） |
| **S2** | 动画解析（json + 二进制）+ chunk 11-16；11 类时间轴全覆盖 | **已完成**（见 7.0.3） |
| **S3** | 槽位 setup（chunk 17）+ color / twoColor / attachment / drawOrder 解算 | **已完成**（见 7.0.4） |
| **S4** | 约束（chunk 18-20）+ update 顺序（前置核对**已完成**，见 §4.1：3.7/3.8 数学差 2 处，按 3.8 实现） | 对拍：376 个有约束的资产子集，逐骨骼比世界变换 |
| **S5** | 多轨混合（`AnimationState` 转录，含 `_timelinesRotation` 最短弧记忆） | 对拍：随机切轨 / 混出，逐帧比对 |
| **S6a** | deform 求解 + per-instance deform 缓冲（方案 C：只给命中项写"deform 后局部坐标"，蒙皮仍在 GPU） | 与 `SpineSkinningParityTest` 接上；断言共享 VB **没被改写** |
| **S6b** | 方案 B：deform 缓冲直接由 GPU 采样，shader 里 rigid 替换 / weighted 相加 | 同上，且逐实例上传量 ≤ 122 B × 命中条数 |

### 7.0.2 S1 已落地（2026-10-22）：骨骼 setup 进容器（chunk 10，格式 v7 → v8）

| 项 | 落地 |
|---|---|
| 布局 | `SpineGpuBone` **36 B/骨**：x, y, rotation, scale_x, scale_y, shear_x, shear_y, length, parent(i16), transform_mode(u8), pad |
| 来源 | 字段集合 `BoneData.h:110-115`；`transform_mode` 取值 `TransformMode.h:34-40`；json 默认值与字段名 `SkeletonJson.cpp:165-189`；二进制顺序 `SkeletonBinary.cpp:157-164` |
| 顺序不变量 | **parent < index** ⇒ 正向 for 一遍就是拓扑序；Bake 与读端**都**校验 |
| 读端校验 | chunk 10 条数 == `header.bone_count`（"chunk 不存在"本身不报错）；`transform_mode ≤ 4`；`pad == 0` |
| 新增报错（参考实现都静默） | 认不出的 `transform` 串；父骨骼不存在（原本会静默变成根骨骼）；`transform_mode` 越界 |
| 落盘顺序 | `1,2,**10**,3,21,22,4,5,9,6,7,8`（C# 照抄） |

**782 个资产重跑**：**781 成功 / 1 失败**，与 v7 完全一致 ⇒ 无回归，
且真实资产里 `transform_mode` 全部落在 0..3（越界那条硬报错没被触发）。

**实测（`doc/scripts/measure_zspg_chunks.py`，781 个容器）**：

| 项 | 值 |
|---|---|
| 骨骼总数 | **32 036**（均 **41.0** 根/资产） |
| 最深树 | **33** 层 |
| chunk 10 合计 | **1.10 MB** |
| transform 模式 | normal **93.68%** / noRotationOrReflection **6.26%** / onlyTranslation 0.04% / noScale 0.02% / **noScaleOrReflection 0** |

⇒ 5 个枚举值里 4 个在真实资产上出现过。`noScaleOrReflection` **0 次**：仍按
`Bone.cpp:172-185` 实现，但**没有真实资产能给它的解算结果背书** —— S4 必须用合成夹具对拍。

`SpineGpuImportTest` 933 → **977/977**；C# 跨语言对拍 219 → **254/254**；
`SpineSkinningParityTest` **11/11 顶点、0.68 ulp**（未受影响）。

**对拍夹具是硬要求**（照 `SpineSkinningParityTest` 9/9、0.68 ulp 的先例）。
动画解算没有对拍的话，"看起来在动"会掩盖所有语义错误。

---

### 7.0.3 S2 已落地（2026-10-22）：动画进容器（chunk 11-16，格式 v8 → v9）

| 项 | 落地 |
|---|---|
| 布局 | chunk 11 动画目录 **20 B/条** → chunk 12 时间轴表 **32 B/条** → chunk 13 帧（float）→ chunk 14 曲线类型（u8/段）→ chunk 15 bezier 控制点 **16 B/段** → chunk 16 变长载荷（字节） |
| 来源 | 11 类时间轴逐类抄 `SkeletonBinary.cpp:719-1107` / `SkeletonJson.cpp:758-1190`；`TimelineType.h:35-49`；`CurveTimeline.cpp:42-44`；各类 ENTRIES 见 `SpineGpuFormat.h::SpineTimelineEntries()` |
| 关键修正 | ⚠️ `frame_first` 的单位是 **float 下标不是"帧"**（§3.2）—— 各类每帧 float 数不同，压根没有统一的"帧"单位。原设计写错了，实现时改掉并同步了 §3.2 |
| 规范化 | 时间轴按 **(type, target) 升序稳定排序**。两条输入路径的顺序本来就不一样（二进制 `SkeletonBinary.cpp:723` 起是槽位→骨骼；json `SkeletonJson.cpp:806` 同序但条目顺序取决于**文件里键的书写顺序**）⇒ 不规范化就无法对拍 |
| 变长三类 | attachment 每帧 1 个 u32（0xFFFFFFFF = 空名）；deform 每帧 `[u32 start][u32 count][f32 × count]`，`count == 0` = 回退帧（抄 `SkeletonBinary.cpp:983-985` 的 `end == 0`）；drawOrder 每帧 `[u32 n][(u32 slot, u32 offset) × n]` |
| 新增报错（参考实现都静默） | 认不出的时间轴类型 / 骨骼名或槽位名不存在 / ik·path 约束名不存在（参考实现把下标**留在 0** ⇒ 动到第一条约束）/ 认不出的 curve 串或数组（参考实现静默变 linear）/ drawOrder 槽位不存在 / 0 帧时间轴（参考实现 `frames[(frameCount-1)*ENTRIES]` 是**负下标**读）/ deform 长度对不上（越界写） |
| 读端校验 | 每条时间轴的帧 / 曲线 / bezier / 载荷区间都查；`bezier_count` 必须等于段里标了 bezier 的段数；动画的 `[timeline_first, +count)` 落在时间表内；`name_offset` 落在字符串块内 |
| 落盘顺序 | `1,2,10,**11,12,13,14,15,16**,3,21,22,4,5,9,6,7,8`（C# 照抄） |

**782 个资产重跑**：**781 成功 / 1 失败**，与 v8 完全一致 ⇒ 无回归
（唯一失败仍是那个资产自己缺图集 region 的真 bug，见 7.0.1）。

**实测（`doc/scripts/measure_zspg_chunks.py`，781 个容器）**：

| 项 | 值 |
|---|---|
| 有动画的容器 | **781 / 781** |
| 动画总数 | **1 932**（均 **2.5** 个/资产） |
| 时间轴总数 | **254 471** |
| chunk 11 动画目录 | 0.04 MB |
| chunk 12 时间轴表 | **7.77 MB**（32 B/条 —— 纯索引开销，源 .skel 里没有这一层） |
| chunk 13 帧数据 | **19.34 MB**（5 070 796 个 float） |
| chunk 14 曲线类型 | **2.45 MB**（2 566 213 段） |
| chunk 15 bezier | **14.32 MB**（938 471 段） |
| chunk 16 变长载荷 | **122.81 MB** |
| **动画块合计** | **166.72 MB** |

时间轴类型分布（15 类里 **11 类**真实出现，S2 的验收口径就是这 11 类）：

| type | 名 | 条数 | 占比 |
|---|---|---|---|
| 6 | deform | 99 721 | **39.19%** |
| 0 | rotate | 53 702 | 21.10% |
| 4 | attachment | 40 007 | 15.72% |
| 1 | translate | 27 298 | 10.73% |
| 5 | color | 22 619 | 8.89% |
| 2 | scale | 9 444 | 3.71% |
| 3 | shear | 1 046 | 0.41% |
| 8 | drawOrder | 174 | 0.07% |
| 11 | pathConstraintPosition | 367 | 0.14% |
| 14 | twoColor | 88 | 0.03% |
| 13 | pathConstraintMix | 5 | 0.00% |
| 7 / 9 / 10 / 12 | event / ik / transform / pathSpacing | **0** | — |

曲线类型分布：**linear 58.26% / bezier 36.57% / stepped 5.17%**
（与 §3.3 的 2 567 358 段实测吻合 ⇒ 紧凑写法省下那 32 MB 是真的省到了）。

deform 载荷：**1 731 940 帧、28 282 880 个 float、均值 16.3 float/帧**
⇒ 确认是**稀疏**的（稠密展开会是 GB 级），但也确认了它是容器里最大的一块。

⚠️ **容器体量的实测结论要更新**（推翻 §8.5 的估算）：
容器合计 **206.32 MB**，源 `.skel.bytes` 合计 **197.28 MB** ⇒ **1.05×**。
动画块占容器的 **80.8%**，其中 deform 载荷独占 **59.5%**。
之前"容器 ≈ 源 .skel 的 75%~80%"那个估算是**错的**：它假设了稀疏化能大幅压缩，
但源 `.skel` 里的 deform **本来就是稀疏的** —— 我们没有在压缩它，只是原样搬了过来，
还额外多了一层 chunk 12 的时间轴索引表（7.77 MB）。

`SpineGpuImportTest` 977 → **1116/1116**；C# 跨语言对拍 254 → **294/294**；
`SpineSkinningParityTest` **11/11 顶点、0.68 ulp**（未受影响）。

---

### 7.0.4 S3 已落地（2026-10-22）：槽位 setup（chunk 17，格式 v9 → v10）+ 槽位侧解算

| 项 | 落地 |
|---|---|
| 布局 | chunk 17 **40 B/槽**：color rgba f32×4 @0 + dark rgba f32×4 @16 + `setup_attachment_name_offset` u32 @32 + `flags` u32 @36（详见 §3.5） |
| 字符串表 | 新增**第 4 组**「槽位 setup 附件名」，`slot_count` 条（**空名也占一条**，否则按 `slot_count` 切分会错位） |
| 新增解析 | 二进制 `ParseSlots()` 原来 `Skip(4)` 把 color **整个丢掉**、还把 `attachment_name` 丢掉 —— 现在两处都真读；json 侧读 `"color"`（8 位 hex）/ `"dark"`（6 位 hex，alpha 恒 1）/ `"attachment"` |
| 新增解算 | `SpineGpuAnimSolver.{h,cpp}`：`SpineGpuSampleSlots(asset, anim_index, time, out)` 出**逐槽位**的 (color, dark, attachment) + 绘制顺序 |
| 落盘顺序 | `1,2,10,11,12,13,14,15,16,**17**,3,21,22,4,5,9,6,7,8`（C# 照抄） |

**⚠️ 先把这个阶段要补的缺口说清楚**：容器里原本**没有槽位颜色**（只有一个 `uses_tint_black`
布尔）。而 color / twoColor 时间轴写的是**绝对**颜色（`ColorTimeline.cpp:110`
`slot._color.set(r,g,b,a)`），"没被时间轴命中"的槽位停在 **setup 色**
⇒ 没有 setup 色就只能默认纯白，而**纯白与 setup 白在多数资产上完全看不出区别**
（典型的"错了但不报错"）。实测（781 个资产）**139 / 781 个资产（17.8%）至少有一个
非纯白 setup 色**的槽位 —— 规模不小，不是理论风险。

#### 采样口径（必须先钉死，否则"看起来在动"会掩盖一切）

解算的语义 = `Animation::apply(..., alpha=1, MixBlend_First, MixDirection_In)`
（`AnimationState.cpp:388` `blend = i==0 ? MixBlend_First : ...`；`:402`）。
alpha == 1 时它就等价于 **"先把所有槽位复位到 setup，再让时间轴覆盖"**。

三条**照抄**的算法（每一条抄错了都不报错）：

1. **帧定位** = `Animation::binarySearch()`（`Animation.cpp:84-106`）。
   ⚠️ 它不是标准二分：`high` 从 `count-2` 起、`high==0` 时直接返回 `step`，
   而且**返回的是后一帧的下标** ⇒ 段号 = `frame / ENTRIES - 1`。
2. **插值参数** = `(time - prev) / (cur - prev)`，再过 `CurveTimeline::getCurvePercent()`
   （`CurveTimeline.cpp:110-141`）：LINEAR 恒等；**STEPPED 恒为 0**（不是"取前一帧"这种
   等价写法能替的）；BEZIER 从 4 个控制点**重建**那 18 个 float 的前向差分表，
   常数是 `0.03f / 0.006f / 0.3f / 0.16666667f`（`CurveTimeline.cpp:78-81`）——
   化简一下它就变成另一条曲线，且完全看不出来。
3. **drawOrder 重建**（`SkeletonBinary.cpp:1040-1072` / `SkeletonJson.cpp:1129-1165`）
   **不是**"把槽位挪到 index+offset"：`drawOrder[originalIndex + offset] = originalIndex`
   用的是**原始（setup）下标**，offset 是**有符号**（zigzag varint），
   没被列出的槽位进 `unchanged`、**从数组尾往回填**。
   ⚠️ 并且槽位下标必须**严格升序**，否则参考实现的 `while (originalIndex != slotIndex)`
   永不终止（死循环 + 越界写）⇒ **Bake 期就拒绝**（新增报错）。

#### 本阶段**故意不做**的（不做 ≠ 做错了）

`SpineGpuAnimSolver.h` 开头写死了范围：**只**做 color / twoColor / attachment / drawOrder。

* **骨骼时间轴**（rotate / translate / scale / shear）→ **S4**（要先有约束与 update 顺序）。
* **deform** → **S6**（共享 VB 不动、蒙皮仍在 GPU，只给命中项写增量）。
* 多轨混合 → S5。

⇒ 现在"骨骼不动、槽位在动"是**预期状态**。留着边界清楚，比"一半对一半猜"好排查。

**782 个资产重跑**：**781 成功 / 1 失败**，与 v9 完全一致 ⇒ 无回归
（唯一失败仍是那个资产自己缺图集 region 的真 bug，见 7.0.1）。

**实测（`doc/scripts/measure_zspg_chunks.py`，781 个容器）**：

| 项 | 值 |
|---|---|
| 槽位总数 | **56 704**（均 **72.6** 个/资产） |
| chunk 17 合计 | **2.16 MB**（容器的 **1.03%**） |
| 带暗色（tint black） | **96** 槽位（0.17%），涉及 **5 / 781** 个资产 |
| setup 色非纯白 | **1 210** 槽位（2.13%），涉及 **139 / 781** 个资产 |
| 有 setup 附件名 | **33 055** 槽位（**58.29%**） |
| flags 有未知位 | **0**（必须 0） |

两点值得记：

* **暗色 96 槽位 / 5 资产**与 §7.0 里 S0 的独立实测（"96 个双色槽位判对"）**对上了** ——
  两条互不依赖的路径给出同一个数，是"3.7 暗色判据正确"的又一条证据。
* **58% 的槽位有 setup 附件名** ⇒ attachment 时间轴（40 007 条，占时间轴 15.72%）的
  复位基线是**常用路径**不是边角，必须落盘。

容器合计 **206.32 MB → 208.79 MB**（+1.2%，就是 chunk 17 那 2.16 MB）；
源 `.skel.bytes` 合计 197.28 MB ⇒ 比值 1.05 → **1.058**。

`SpineGpuImportTest` 1116 → **1214/1214**；C# 跨语言对拍 294 → **323/323**；
`SpineSkinningParityTest` **11/11 顶点、0.68 ulp**（未受影响）。

**解算侧的断言怎么写才有用**（三条都踩过）：

1. 期望值**手算**，不从实现里抄回去 —— 只有这样才能抓到"两边一起错"。
2. 夹具必须有**非默认**值：全是纯白的往返什么也挡不住（读回来还是纯白 ⇒ 通过）。
   ⇒ `kSkeleton` 的 s1 带 `"color": "80c0ff40"` + `"dark": "204060"`。
3. **读回来的容器必须解算出同一个结果**（`asset` 与 `back` 在 6 个时间点上逐分量比）。
   写端对了、读端错了，往返的逐字节断言**全**看不出来 —— 只有解算能抓到。

---

### 7.0.5 S4a 已落地（2026-10-23）：约束 setup 进容器（chunk 18 / 19 / 20，格式 v10 → v11）

| 项 | 落地 |
|---|---|
| 布局 | chunk 18：**IK 20 B** / **transform 56 B** / **path 40 B** + 三类**共用**的 `uint16` 骨骼池（块头 16 B）；chunk 19：**4 B/步** `SpineGpuUpdateStep` + `uint16` 复位表（块头 8 B）；chunk 20：**40 B/条** PathAttachment + 骨骼池 + 顶点 float + length float（块头 16 B）。逐字段布局与四条坑见 `doc/GPU_SPINE_INTERCHANGE_FORMAT.md` §3.7 |
| 字符串表 | 新增**第 8 组**「path 附件名」，`att_count` 条，**不去重**，与 chunk 20 记录一一对应。⚠️ 必须排在「动画附件名」**前面**（后者是"剩下的全归它"，没有计数，排在它后面的组读端切不出来） |
| 新增解析 | 二进制 `ParseConstraints()` 补齐 IK / transform / path 三段的真读（原来整段 Skip）；json 侧读 `"ik"` / `"transform"` / `"path"` 三个数组。两种输入都补了 v11 源字段的清空（原来只有 json 侧清） |
| **新增 Bake** | `BakeSpineConstraints()`：三类约束 + 骨骼池 + **`updateCache()` 的执行顺序与复位表** + PathAttachment 几何 |
| 落盘顺序 | `1,2,10,11,12,13,14,15,16,17,**18,19,20**,3,21,22,4,5,9,6,7,8`（C# 照抄） |

**⚠️ 先把这个阶段要补的缺口说清楚**：容器里原本**没有任何约束数据**。而约束在
`Skeleton::updateWorldTransform` 里**每帧都跑**、与有没有时间轴无关，缺了它们的表现是
**姿势不动**，不报错。实测 **481 / 781（61.6%）** 个资产带约束（transform 2082 条、
IK 344 条、path 333 条）—— 这是 §1.3 那条硬边界的落地。

#### 三条实测出来的坑（全是"静默错"，全靠 skel↔json 对拍抓到）

1. **json 的布尔必须当 0/1 读**。Spine 的 json 解析器把 `true/false` 写进 `_valueInt`
   （`Json.cpp:165,173`），而 `getInt()` 读的正是它（`Json.cpp:89-91`）。
   我们的 `GetInt` 只认 `IsNumber()` ⇒ `bendPositive:false` 回落默认 **1**（方向反了）、
   `constantSpeed:false` 回落 1。实测对拍结果：json 侧 `bend=1,flags=0` 对 skel 侧
   `bend=-1,flags=5`。**只有逐字段对拍抓得到** —— 单看 json 一路，结果完全"合理"。
2. **同一槽位挂两条 path 附件是合法的**。我最初按"重复 ⇒ 数据坏了"报错，一次卡掉
   **12 个真资产**（769/41，对不上 781/29 基线）。回查 `Skeleton.cpp:557-635` 的
   `sortPathConstraint` 会遍历**全部** ⇒ 撤回报错，改成两条都烘焙、名字进字符串表、
   `path_index` 按**名字 == 槽位的 setup 附件名**解析。恢复 781/29。
   ⚠️ "取第一条"在单条时与"按名字取"恒等 ⇒ **夹具必须放两条**才分得开。
3. **chunk 19 只在有约束时落盘**。无约束时 `updateCache()` 的结果**可证恒等于**
   `0..bone_count-1`（`parent < index` ⇒ 正向遍历就是拓扑序）、复位表为空。
   读端按同一规则重建 ⇒ Bake 内存与 Read 内存一致、读→写仍逐字节相同。
   781 个资产里 **300 个**省掉了纯冗余数据。
   反向约束：有 chunk 18 却没 chunk 19 ⇒ **容器坏了，必须报错**（静默退化成"没有约束"，
   与"约束没排进去"在字节上完全看不出区别）。

#### 实测（`doc/scripts/measure_zspg_chunks.py`，781 个容器）

| 项 | 值 |
|---|---|
| 带约束的容器 | **481 / 781（61.6%）** |
| IK / transform / path | **344** / **2082** / **333** 条 |
| 约束骨骼池 | **3150** 条 |
| 执行顺序 | **27 468** 步（bone 89.96% / transform 7.58% / ik 1.25% / path 1.21%） |
| 约束给遍历表加的步数 | 带约束资产共 24 941 根骨骼 ⇒ +**2 527** 步（**9.2%**） |
| 复位骨骼 | **266** 条 |
| PathAttachment | **690** 条（11 592 个顶点 float、1 921 个 length float、77 个 u16 骨骼） |
| `path_index` 有效 | **333 / 333（100%）** ⇒ 这些资产的 path 约束在 setup 状态下就生效 |
| **三块合计** | **0.33 MB**（容器的 **0.16%**） |
| 健康度 | pad 非 0 / bend_dir 非法 / kind 越界 / order 非排列 **均为 0** |

path 的 mode 分布：position **percent 100%**；spacing **length 99.10%** / percent 0.90%；
rotate **tangent 99.10%** / chainScale 0.90% ⇒ 固定值模式几乎没用到，但**编码仍要保留**
（编码本身不花钱，砍掉它省不了字节却会卡住未来的资产）。

**落盘的代价可以忽略（0.16%），不落的代价是姿势静默不动。**

**782 个资产重跑**：**781 成功 / 29 失败**，与 v10 基线**完全一致** ⇒ 无回归
（29 个失败全是"找不到同名图集"的配置噪音，0 个 Bake 失败）。

`SpineGpuImportTest` 1214 → **1283/1283**；C# 跨语言对拍 **PASS**；
`ctest -C Debug -R SpineGpu` **2/2**。

#### 本阶段**故意不做**的（不做 ≠ 做错了）

**约束解算本身 → S4b**。现在容器里有完整的约束 setup 与执行顺序，但
`SpineGpuAnimSolver` **还不会用它们** —— 骨骼仍然不动。留着边界清楚，
比"一半对一半猜"好排查。S4b 的范围：

1. 骨骼时间轴（rotate / translate / scale / shear）的采样 —— 复用 §7.0.4 那套
   帧定位 + 曲线插值口径，不另写一套。
2. 三类约束的求解，**按 3.8 的数学**（§4.1：两处 3.7 bug 我们不复现）。
3. 按 chunk 19 遍历（含复位表），而不是自己重排。
4. 对拍：`build/spine_ref_probe` 编出的 3.8 `SpineReference.lib` 与 `/i/spine-runtimes`
   的 3.7.83，在带约束的那 481 个资产子集上逐骨骼世界变换比。

---

### 7.0.6 S4b 在途（2026-10-22）：解算器已写出 + 参考实现对拍台已通

**代码**：`SpineGpuPoseSolver.h/.cpp`（新增）导出 `SpineGpuSamplePose(asset, anim, time, out)`，
一次调用给出该帧每根骨骼的 `(a,b,c,d,worldX,worldY)`。
`SpineGpuAnimSolver` 里的 `SpineGpuBinarySearchFrames` / `SpineGpuCurvePercentAt`
**提成了导出符号**，让槽位解算与骨骼解算共用**同一份**帧定位实现（否则"两边各写一遍
二分查找"迟早会漂）。

执行顺序（照 `Animation::apply` + `Skeleton::updateWorldTransform`）：
骨骼复位到 setup → **先采样槽位** → 骨骼时间轴 → 复位表 → 按 chunk 19 遍历。
⚠️ 槽位必须在骨骼**之前**：`PathConstraint::update()` 读的是
`_target->getAttachment()`（PathConstraint.cpp:77），而 attachment 时间轴会改它。

**对拍台**：`Test/SpineGpuRefParityTest.cpp` —— 把 OSGame 引擎里带的 spine-cpp **3.8**
（`G:/OSGame/unity-engine-source/Modules/Spine/spine-cpp`）编成 `SpineReference.lib`
（排除 `Atlas.cpp` / `AtlasAttachmentLoader.cpp`：前者吃 Unity 的 baselib 头，
而 `BaselibPlatformSpecificEnvironment.h` 在引擎里**根本不存在**；代价是要给
`AtlasAttachmentLoader` 补一份"永不调用"的桩，以及补 spine-cpp 留白给宿主的
`getDefaultExtension()`），然后逐资产 × 逐动画 × 5 个时间点 × 每根骨骼比 6 个分量。
参考路径用空 `AttachmentLoader`（只比骨骼，不需要图集）。

**为什么必须有这一条**：IK / transform / path 是**迭代求解器**，期望值**手算不出来**。
夹具里写死期望值只会把实现的行为抄回给自己 —— 正是 §12「期望值手算，不从实现里抄回去」
要防的循环论证。唯一能真正钉住转录错误的是**把参考实现跑起来比**。

**实测（810 个 .skel.bytes / .json，781 个烘焙成功）**：

| 指标 | 值 |
| --- | --- |
| 逐骨骼比对通过的文件 | **706 / 781** |
| 动画 × 时间点采样 | 8 957 |
| 骨骼分量比较次数 | 2 267 244 |
| 通过部分的最大相对误差 | ~6e-6 |

**对拍台立刻抓到的两个 bug**（都是"静默"型，靠夹具永远测不出来）：

1. **chunk 20 的池偏移没用**（已修）。`PathComputeWorldVertices` /
   `PathComputeWorldPositions` 直接用 `verts[i]` / `lengths[i]`，漏掉了
   `path_attachment.vertex_offset` / `length_offset`。chunk 20 的顶点池与 lengths 池是
   **所有 path 附件共用**的（`SpineGpuImporter.cpp:5244/5247`），偏移单位是 **f32 下标**。
   漏掉它 ⇒ 每条 path 都读第 0 条曲线的控制点 ⇒ 所有 path 约束把骨骼摆到**同一个点**。
   ⚠️ 这正是"偏移为 0 的那次测不出来"的老坑：资产只有一条 path 时完全正确。
2. **deform 会改 path 的形状，进而改骨骼位置**（**未修，占 62 / 75**）。
   `VertexAttachment::computeWorldVertices()` 在 **rigid** 分支里会
   **优先用槽位的 deform 数组整体替换顶点**（VertexAttachment.cpp:68-70），
   而我们目前**跳过** deform 时间轴（理由是"它只影响顶点、在 GPU 上做"）。
   对普通 mesh 这个理由成立；**对 path 附件不成立** —— deform 改的是约束的**输入**，
   骨骼位置会跟着变。⇒ S4b 必须给"path 约束的目标槽位"在 CPU 上做 deform。

**状态**：解算器可编译、可跑、706/781 对得上；**尚未**接到渲染侧，也**没有**注册进
ctest（它依赖 `G:/OSGame` 的引擎源码，是加码不是门禁）。

---

## 8. 风险 / 未决

1. ~~**deform 的取舍需拍板**~~ —— **已定**：稠密展开否决、OSGame 式全量 CPU 动态 VB 否决，
   走「共享 bind pose + deform 增量」（§5.2）。剩下的只是 B/C 谁先落地，**不改变 v7 体量**。
2. ~~**约束解算的参考源**：3.7/3.8 的约束**数学**是否一致尚未逐行核对（只核对了字节布局）~~
   —— **已核对完（2026-10-22，见 §4.1）**：10 个文件里只有 **2 处**语义差异，两处都是
   3.8 修掉的 3.7 bug（`Bone.cpp` 根骨骼分支的 sx/sy 用反；`IkConstraint.cpp` 单骨骼分支
   的运算符优先级 ⇒ `b≈0` 时 NaN）。**S4 按 3.8 的数学实现**。
   ⚠️ 核对方法本身有个坑：不剥掉文件头的 license 注释，10 个文件每个都有 500+ 行
   "差异"，真正的 2 处会被完全淹掉。
3. **双色（tint black）**：~~96 槽位 / 5 文件，规模很小，但要在渲染侧多一路颜色~~
   —— **数据侧已落地**（S3：chunk 17 的 `has_dark` flag + dark rgba，实测 96 槽位 / 5 资产，
   与 §7.0 的独立实测对上）。**剩下的只是渲染侧**：走双色分支要第二路颜色，
   目前还没有接。⇒ S4 之后、接渲染时补。
4. **多皮肤 = 0**：v2 的皮肤维度在本工程没用上。不删（换装是容器承诺的能力），
   但也说明皮肤相关的测试覆盖对 OSGame 没价值。
5. **容器体量**：~~按 §3.3 的紧凑曲线 + 稀疏 deform 粗估，容器 ≈ 源 .skel 的 75%~80%~~
   —— **估算已被实测推翻**（见 7.0.3，S3 后重测见 7.0.4）：
   容器 **208.79 MB** vs 源 **197.28 MB** = **1.058×**（v10 的 chunk 17 加了 2.16 MB）。
   动画块占 80.8%，deform 载荷独占 59.5%（28.28 M 个 float）。
   下一个真正可压的点是 **deform 载荷**（f32 → fp16 或 i16 定点，理论 −50%），
   但它会引入精度分歧，且与 S6 的 deform 求解绑在一起 ⇒ **等 S6 落地后再动**。
