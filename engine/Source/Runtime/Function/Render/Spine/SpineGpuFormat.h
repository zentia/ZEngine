#pragma once

// =====================================================================
// GPU-Spine 中间格式 —— 数据布局的唯一权威定义
// 规格：doc/GPU_SPINE_INTERCHANGE_FORMAT.md（草案 v0.1）
// ---------------------------------------------------------------------
// 本头文件**只定义数据长什么样**，不含导入逻辑、不含渲染逻辑、不依赖 RHI。
// ZEngine 导入器（C++）与 Unity 导入器（C#）必须产出逐字节相同的容器。
//
// 为什么每一个偏移都要 static_assert：
//   顶点布局错一个字节**不会**报错 —— 编译得过、跑得起来、画得出来，
//   只是形状静默错。这类 bug 在运行时几乎无法定位，只能在编译期挡住。
//   （和 ShaderLabUnityCompatTest 用 [0f] 钉 GBuffer 布局是同一个道理。）
//
// ⚠️ 版本锁定：Spine **3.8**（规格 §1.1）。
//   骨骼索引是 **uint16**，不是 uint32 —— 因为 3.8 的 spine-cpp 里
//   `_bones` 声明为 `Vector<unsigned short>`（VertexAttachment.h:79-81，
//   源码注释："unsigned short is ample; a skeleton cannot reach 65535 bones."）。
//   升 4.x 时必须回来改这里（4.x 是 `Vector<int>`，但**未在本机核实**，别照抄这句）。
// =====================================================================

#include <cstddef>
#include <cstdint>
#include <type_traits>

// ---------------------------------------------------------------------
// 常量
// ---------------------------------------------------------------------
namespace SpineGpuConstants
{
    constexpr uint32_t kMagic = 0x5A535047u;   // 'ZSPG'

    // 版本 2（2026-10-21）：容器补上 **skin 维度** ——
    //   * SpineGpuHeader 末位的 reserved 改成 skin_count
    //   * 附件记录从 4 B（slot, attachment）扩成 8 B（slot, attachment, skin）
    //   * SpineDrawItem 尾部 2 B 填充改成 skin_id
    // 版本 3（2026-10-21）：**UV 拆成独立通道** ——
    //   * 顶点里不再有 uv：rigid 16 B → 8 B，weighted 56 B → 48 B
    //   * 新增 chunk 4（uv_buffer），绘制项新增 uv_offset（⇒ 绘制项 20 B → 24 B）
    //   * chunk id 重新编号（uv 插在顶点缓冲后面）
    //   目的：顶点只存"几何 + 蒙皮"，几何相同的附件可以**共用**同一段顶点，
    //   UV 各取各的（规格 §14.2 —— linked mesh / 换装变体的几何是重复的）。
    // 版本 4（2026-10-21）：**deform（FFD）参与烘焙** ——
    //   * 布局**没变**（flags 多一个 kHasDeform 位，绘制项的 kind 字段本来就有）
    //   * 但语义变了：被 deform 时间轴命中的附件，绘制项 kind 从 kStatic 变成 kFallback
    //   ⇒ v3 的容器会把带 FFD 的附件标成 kStatic，画出来是**静默错的**（顶点不动）
    //   所以照样是**不兼容**改动：旧容器必须被明确拒绝，不做静默兼容（规格 §11）。
    // 版本 5（2026-10-21）：**裁剪作用域参与烘焙** ——
    //   * 布局还是没变（作用域不落盘：它已经表达成"作用域内绘制项的 kind = kFallback"）
    //   * 但语义变了：落在裁剪作用域里的绘制项，kind 从 kStatic 变成 kFallback
    //   ⇒ v4 的容器会把这些项标成 kStatic，画出来是**不裁剪**的（静默错），
    //   所以同样是不兼容改动（规格 §7.2、§11）。
    // 版本 6（2026-10-21）：**裁剪多边形进容器**（chunk 9 不再是空的）——
    //   * 绘制项 24 B → **28 B**（多了 clip_index：这一项被哪条裁剪多边形裁）
    //   * chunk 9 = SpineGpuClipPolygon[]，12 B/条
    //   * 多边形顶点**复用主顶点缓冲**（chunk 3）：裁剪附件是 VertexAttachment，
    //     顶点与 mesh 顶点同构（rigid / weighted 两种），连去重都能一起做
    //     ⇒ 不需要新的段信息，header 64 B 保持不动。
    //   为什么不是"和 deform 一样不烘焙"：deform 是**逐帧动画数据**（来自时间轴），
    //   而裁剪多边形是**静态资产几何**（局部坐标 + 骨骼绑定），与顶点同类 ——
    //   顶点烘焙了它不烘焙，容器就还是不自包含。
    // 版本 7（2026-10-22）：**蒙皮数据从顶点里搬出去，改成逐顶点变长** ——
    //   * weighted 顶点 48 B → **8 B**：只剩一条引用 (influence_offset, influence_count)
    //   * 真正的 (bindPos, boneIdx, weight) 进 **chunk 21 / 22**（影响流）
    //   * 顶点数、绘制项大小、header 大小**都没变**
    //   ⇒ 但旧容器用新读端读会把"引用"当成 bindPos ⇒ 形状全错且不报错，
    //     所以照旧是**不兼容**改动（规格 §11）。
    //   动因是 OSGame 782 个资产实测（doc/GPU_SPINE_V7_ANIMATION.md §1.5）：
    //   每顶点影响数最大 **15**、均值 **2.20**，定长 4 槽卡死 297 个资产（4.1%）；
    //   定长 16 槽能装下但要 176 B/顶点，与"降内存"的目标相反。
    //   变长后均值 ≈ 8 + 12 × 2.20 ≈ **34 B/顶点**（0.72×），且 shader 循环
    //   2.2 次而不是固定 4 次 ⇒ **内存与 ALU 一起省**。
    // 版本 8（2026-10-22）：**骨骼树进容器**（chunk 10，36 B/骨）——
    //   * header 没变（bone_count 本来就在 0x10），绘制项 / 顶点布局都没动
    //   * 但"容器里没有骨骼 setup"这件事本身就是不自包含：动画（S2）与时间轴
    //     都是**相对 setup pose** 的增量，没有 setup 就无从解算
    //   ⇒ 不兼容：v7 容器被新读端读会得到 0 根骨骼（chunk 10 不存在），
    //     所有蒙皮索引全部越界，而"没有 chunk 10"本身不报错。
    // 版本 9（2026-10-22）：**动画进容器**（chunk 11-16）——
    //   * header 与既有 chunk 的布局**全都没动**（动画数量由 chunk 11 的大小反推，
    //     不需要往已经排满的 64 B header 里再塞字段）
    //   * chunk 11 动画目录（20 B/条）→ chunk 12 时间轴表（32 B/条）→
    //     chunk 13 帧（float）→ chunk 14 曲线类型（u8/段）→
    //     chunk 15 bezier 控制点（16 B/段）→ chunk 16 变长载荷（字节）
    //   * flags 多一位 kHasEventTimeline（事件时间轴**不烘焙**，但不许静默当没看见）
    //   ⇒ 不兼容：v8 容器被新读端读会得到 0 个动画（chunk 11 不存在），
    //     而"没有动画"本身不报错 —— 与 chunk 10 是同一类坑。
    // 版本 10（2026-10-22）：**槽位 setup 进容器**（chunk 17，32 B/槽）——
    //   * 之前容器里只有一个 `kUsesTintBlack` 布尔、**没有颜色值**。而 color /
    //     twoColor 时间轴是**相对 setup 颜色**混合的（`ColorTimeline::apply` 直接
    //     往 `slot._color` 上写绝对颜色，setup 色则是"没有时间轴命中"时的基线）⇒
    //     容器里没有 setup 颜色就无从解算。这是一个既有缺口，不是新增需求。
    //   * header / 绘制项 / 顶点布局全都没动（槽位数本来就在 header 0x14）。
    //   ⇒ 不兼容：v9 容器被新读端读会得到"所有槽位纯白 + 无暗色"，
    //     而"纯白"本身不报错 —— 与 chunk 10 / 11 是同一类坑。
    // * v11 = 约束 setup 进容器（chunk 18 约束 / 19 执行顺序 / 20 path 几何）。
    //     header 与既有 chunk 全都没动；三个 chunk 都**自带计数头**，所以
    //     "chunk 不存在"读端看到的是"0 条约束"而不是报错 —— 与前面同一类坑，
    //     所以仍然是不兼容升级。
    //     ⚠️ 执行顺序（chunk 19）**直接落参考实现 `updateCache()` 的结果**，
    //        不落"让运行时自己重排"：重排规则是 `sortBone` 的递归 +
    //        约束 order 两层，运行时再实现一遍就是给自己多一次写反的机会。
    // * v12 = deform 时间轴可以指向 **path 附件**（`target` 最高位，见
    //     `kDeformTargetPathBit`）。布局一个字节都没变，但**语义**变了：v11 读端会把
    //     0x8000+k 当成附件记录下标（越界 / 读到别人），v11 容器被新读端读则"path 的
    //     deform 恒不存在"—— 两个方向都不报错，所以照规矩升版本。
    // * v13 = 绘制项的 `uv_offset` 改成 `uv_unit - vertex_base`（uint32 回绕），让规格 §3.2 的
    //     `uv_buffer[uv_offset + index]` 在索引是**段内绝对**下标时成立。布局不变、语义变：
    //     v12 的值只在 vertex_base == 0 时对（OSGame 112 562 / 119 737 个绘制项不是），
    //     错 UV = 贴图采错块，不报错。读端逐索引校验 `uv_offset + index < uv_count`。
    //     同一版本里绘制项的另外两个字段也**第一次写实**：`blend_mode`（取自槽位，以前恒
    //     kNormal ⇒ 加色特效画成普通混合）与 `atlas_page`（取自 region，以前恒 0）。
    // * v14 = 绘制项 28 → **32 B**：`vertex_base`（u16 @26）+ `deform_parent`（u16 @28）+
    //     `vertex_count`（u16 @30）。三者都是 GPU 上做 mesh deform（S6）的前提，且**都无法从别的
    //     字段反推**（vertex_count：deform 数组长度 = rigid 2×顶点数 / weighted 2×影响数，而
    //     "最大索引 + 1"在有未被三角形引用的尾部顶点时会少算）：
    //       · deform 数组按**附件本地**下标排（rigid 每顶点 2 float，weighted 每影响 2 float），
    //         而 shader 拿到的是段内**绝对**下标 ⇒ 要减 vertex_base（v13 只把它折进了
    //         uv_offset，单独取不回来）；
    //       · `MeshAttachment::applyDeform()`（MeshAttachment.cpp:94-95）：时间轴作用于
    //         `this == source || (inheritDeform && parentMesh == source)` ⇒ linked mesh 要知道
    //         "继承谁的 deform"。烘焙期的 inheritDeform 传播只决定 kind，运行时**认不出**
    //         挂父附件 deform 的那条时间轴该不该作用于自己。
    // * v15 = 裁剪多边形记录 12 → **20 B**：`name_offset`（u32 @12，字符串表第 9 组）+
    //     `end_slot`（u16 @16）；deform 时间轴的 `target` 多一个 **clip 位**（0x4000）。
    //     三者都是"运行时按**当前**附件跑裁剪状态机"的前提，v14 及以前只在烘焙期按 setup
    //     附件、setup 绘制顺序模拟一遍，结果折成绘制项的 `kind` / `clip_index`：
    //       · 附件名：attachment 时间轴按**名字**换槽位附件 ⇒ 运行时要认出"槽位此刻挂的是不是
    //         这条裁剪附件"（与 v11 path 附件名同一个道理；一个槽位挂两条裁剪附件也合法）；
    //       · 结束槽位：`clipEnd(slot)` 只在 `slot == _endSlot` 时收尾（SkeletonClipping.cpp:72-76），
    //         drawOrder 时间轴一改顺序，作用域就跟着变 ⇒ 烘焙期的作用域在动画里不成立；
    //       · 绘制项 @23 的 pad 变成 `item_flags`，bit0 = region：CPU 裁剪要按参考实现的顶点顺序喂
    //         `clipTriangles()` —— `RegionAttachment::computeWorldVertices` 出的是 BR, BL, UL, UR
    //         （RegionAttachment.cpp:137-158），容器里烘的是 `_vertexOffset` 的 BL, UL, UR, BR；
    //         不裁时两种对角线画出来一样，裁了之后三角形拓扑就不一样了。4 顶点 6 索引的 mesh
    //         与 region 长得一模一样 ⇒ 只能落一个位，不能猜；
    //       · clip 位：`ClippingAttachment` 是 VertexAttachment ⇒ deform 改的是裁剪多边形
    //         （clipStart 里 `computeWorldVertices(slot, ...)` 读槽位的 deform 数组）。
    //         v14 在烘焙期当"不影响姿势"跳过 —— 对骨骼成立，对**画面**不成立：
    //         实测 OSGame 有 7 条 deform 时间轴指向裁剪附件（K_Niumo `shandian1` 等）。
    constexpr uint32_t kFormatVersion = 15;

    // **id 0 恒为 default 皮肤**。
    // 两个来源都保证这一点：二进制格式的 default skin 就是先读的那一个
    // （SkeletonBinary.cpp:276，名字写死 "default"）；Spine 编辑器导出的
    // json 里 default 也总排第一。
    // 但"保证"不等于"可以不管"：导入器会**显式检查**名为 default 的皮肤
    // 是否真的在首位，不在就报错 —— 悄悄挪位会让"默认外观"变成另一个皮肤，
    // 而外观错了没有任何报错，正是最难查的那类 bug。
    constexpr uint32_t kDefaultSkinId = 0;

    // 容器里记录的 spine-cpp 版本：3.8.xx -> 30800。
    // 必须记录，因为 3.8（u16 骨骼索引）与 4.x（int）布局不同（规格 §9、§11）。
    constexpr uint32_t kSpineRuntime38 = 30800;

    // 3.7.xx -> 30700。导入器接受 3.7 与 3.8：两者的 .skel 字节布局逐段比对完全一致，
    // 只有槽位的暗色判据不同（3.7 `darkColor != -1`；3.8 四分量不全为 0xff）。
    // ⚠️ 必须**写实**：把 3.7 资产记成 30800 会让读端按 3.8 的判据去解释暗色，
    // 而那个差别不会报错，只会让少数槽位颜色不对。
    constexpr uint32_t kSpineRuntime37 = 30700;

    // 每顶点影响数上限。导入器遇到 n > 4 必须**报错并报出附件名**，
    // 不允许静默截断 —— 静默截断 = 形状悄悄错（规格 §3.1）。
    //
    // ⚠️ v7 起它**不再是"固定槽位数"**，而是"单个顶点影响数的硬上限"。
    // 定长 4 槽的写法在 OSGame 实测下不成立：每顶点影响数最大 **15**、均值 2.20，
    // >4 的顶点占 4.1%（24 458 / 594 372）⇒ 297 个资产直接导不出来。
    // 改成**逐顶点变长**（chunk 21/22）后，这个常量只用来挡"数据坏了"
    // （比如把文件读错段、把 n 读成了 40 亿），正常资产永远碰不到它。
    constexpr uint32_t kMaxInfluences = 255;

    // 骨骼表步长：6 float 紧凑（默认）或 32 B（std140 UBO 兼容变体）。
    constexpr uint32_t kBoneStrideCompact = 24;
    constexpr uint32_t kBoneStrideStd140 = 32;

    // 根变换步长。root 与骨骼同构（都是 2×3 仿射），std140 变体跟着 bone_stride 走。
    constexpr uint32_t kRootStrideCompact = 24;
    constexpr uint32_t kRootStrideStd140 = 32;

    // 每帧实例项大小（含 root_index，规格 §5.2）。
    constexpr uint32_t kDrawInstanceSize = 20;

    // 静态绘制项大小（规格 §4）。v3 起是 24 B（多了 uv_offset），v6 起是 28 B（多了 clip_index），
    // v14 起是 32 B（多了 vertex_base / deform_parent）。
    // 与 kDrawInstanceSize 别互相换用 —— 一个是静态表项、一个是每帧实例项。
    constexpr uint32_t kDrawItemSize = 32;

    // deform_parent 的"没有"值（不是 linked mesh，或 linked mesh 的 inheritDeform = false）。
    // 附件数上限是 0xFFFF 条（烘焙期校验）⇒ 合法 id 最大 0xFFFE，0xFFFF 不会撞上。
    constexpr uint16_t kNoDeformParent = 0xFFFFu;

    // 裁剪多边形记录大小（chunk 9，规格 §7.2）。v6 起 12 B，v15 起 20 B（多了 name_offset / end_slot）。
    constexpr uint32_t kClipPolygonSize = 20;

    // 裁剪多边形 `end_slot` 的"没有"值：json 没写 "end" ⇒ `_endSlot` 是 NULL ⇒ `clipEnd(slot)`
    // 永不成立 ⇒ 一路裁到绘制顺序末尾（SpineMesh.h:259 那次无条件 `clipEnd()` 收尾）。
    constexpr uint16_t kNoClipEndSlot = 0xFFFFu;

    // clip_index 的"没有"值。绘制项里存的是 chunk 9 的行号，
    // 用 0xFFFF 而不是 0 —— 0 是一条**真实存在的**多边形，用它会静默裁错。
    constexpr uint16_t kNoClipIndex = 0xFFFFu;

    // 顶点步长。**UV 已经不在这里了**（v3 起它在独立的 chunk 4 里）⇒
    //   rigid    : bindPos 2 float                                  = 8 B
    //   weighted : (influence_offset u32, influence_count u32)      = 8 B   ← v7 起
    //
    // ⚠️ v7 把 weighted 从"定长 48 B"改成"**逐顶点变长**"：顶点只存一条**引用**
    // （影响流的起始条目 + 个数），真正的 (bindPos, boneIdx, weight) 进 chunk 21/22。
    // 理由（OSGame 782 个资产实测，见 `doc/GPU_SPINE_V7_ANIMATION.md` §1.5）：
    //   * 定长 4 槽不够 —— 实测最大 15 个影响，>4 的占 4.1% ⇒ 297 个资产导不出来；
    //   * 定长 16 槽能容纳全部，但要 176 B/顶点（48 B 的 3.7×），**与降内存的目标相反**；
    //   * 变长后均值 ≈ 8 + 12 × 2.20 ≈ **34 B/顶点**（48 B 的 0.72×），
    //     且 shader 循环 2.2 次而不是固定 4 次 ⇒ **内存和 ALU 一起省**。
    constexpr uint32_t kRigidVertexStride = 8;
    constexpr uint32_t kWeightedVertexStride = 8;

    // 影响流（chunk 21 / 22）每条的大小。**两条流分开**而不是交错成一条 12 B：
    // `ByteAddressBuffer::Load2` 要 8 字节对齐，12 B 步长会让第 1、3、5…条落在
    // 12、36…（不是 8 的倍数）⇒ 只能退化成标量 Load。分开后 bindPos 流恒定 8 B 对齐。
    constexpr uint32_t kInfluenceBindStride = 8;    // float2 bindPos
    constexpr uint32_t kInfluenceBoneStride = 4;    // u16 boneIdx + u8 weight + u8 pad

    // 一条影响 = bindPos(8) + bone(4)。**只用在导入期的去重池上**（当"键"用），
    // 落盘时拆成 chunk 21 / 22 两条流 —— 12 B 步长会破坏 Load2 的 8 B 对齐。
    constexpr uint32_t kInfluenceEntrySize = 12;

    // UV 条目：2 float，8 B。它自己是 8 B 对齐的 ⇒ Load2 直接可用。
    // 注意 UV 条目的个数**不等于**顶点数：同一段顶点可以被多个绘制项共用，
    // 每个绘制项各有一份 UV（这正是拆通道的目的）。
    constexpr uint32_t kUvStride = 8;

    constexpr uint32_t kHeaderSize = 64;
    constexpr uint32_t kChunkTableEntrySize = 16;

    // 顶点缓冲每段起点必须 8 B 对齐（同上：为了对齐的 Load2）。
    constexpr uint32_t kVertexSegmentAlignment = 8;
    // 其余块 4 B 对齐。
    constexpr uint32_t kChunkAlignment = 4;

    // 附件记录（chunk 7）大小。4 B → 8 B 是因为多了一列 skin_id。
    constexpr uint32_t kAttachmentRecordSize = 8;

    // 骨骼 setup 记录（chunk 10）大小：7 float + length + int16 parent + u8 mode + pad。
    // 字段集合抄 `BoneData.h:110-115`，transform_mode 取值抄 `TransformMode.h:34-40`。
    constexpr uint32_t kBoneSetupSize = 36;

    // 动画目录项（chunk 11）大小。
    constexpr uint32_t kAnimDirSize = 20;

    // 时间轴表项（chunk 12）大小。
    constexpr uint32_t kTimelineEntrySize = 32;

    // 一条 bezier 段的控制点（chunk 15）大小：4 个 float。
    // ⚠️ 存**控制点**不存参考实现那 18 个 float 的展开采样表
    //    （`CurveTimeline::BEZIER_VALUES = 10*2-2`）：实测 2 567 358 段里 bezier 占
    //    36.6%，定长 20 B/段要 49.0 MB，紧凑写法 16.8 MB（差 65%）。
    constexpr uint32_t kBezierSize = 16;

    // 槽位 setup 记录（chunk 17）大小：
    //   color rgba f32×4 @0 + dark rgba f32×4 @16 +
    //   setup_attachment_name_offset u32 @32 + flags u32 @36。
    // 字段集合抄 `SlotData.cpp:39-41`（color 默认 **1,1,1,1**、dark 默认 0,0,0,0）；
    // 二进制读法抄 `SkeletonBinary.cpp:182-189`，json 读法抄 `SkeletonJson.cpp:219-232`。
    //
    // 32 B 不够：还要一格放 **setup 附件名偏移**（attachment 时间轴的复位基线，
    // `AttachmentTimeline.cpp:65-68`）和一格放 **逐槽位 flags**（has_dark ——
    // 它**不能**由 dark 是否全 0 推断：3.7 的暗色判据是 `int32 != -1`，
    // 0x00000000（纯黑）照样是"有暗色"，见 `SpineGpuSlot` 的注释）。
    // 40 = 5 × 8 ⇒ 仍是 8 B 对齐，结构化缓冲可以整条 4 B 读。
    //
    // ⚠️ dark 的 alpha **恒为 1**（`SkeletonBinary.cpp:188` 的 `set(r,g,b,1)`、
    //    `SkeletonJson.cpp:230` 的 `darkColor.a = 1`）—— 二进制里读进来的第 4 个字节
    //    只用来判"有没有暗色"（3.8 判据：四分量不全为 0xff），**从不作为 alpha 落地**。
    //    照抄这个不对称，别把它当"读漏了一个字节"去补。
    constexpr uint32_t kSlotSetupSize = 40;

    // ---- v11 约束（chunk 18 / 19 / 20）----
    constexpr uint32_t kIkConstraintSize = 20;
    constexpr uint32_t kTransformConstraintSize = 56;
    constexpr uint32_t kPathConstraintSize = 40;
    constexpr uint32_t kUpdateStepSize = 4;
    constexpr uint32_t kPathAttachmentSize = 40;
    // chunk 18 / 19 / 20 各自带一个"计数头"，所以"chunk 不存在"读端看到的是
    // "0 条"而不是报错 ⇒ 三个 chunk 的**条数必须与其它 chunk 对得上**（见读端校验）。
    constexpr uint32_t kConstraintHeaderSize = 16;   // ik_count, tf_count, path_count, bone_pool_count
    constexpr uint32_t kUpdateOrderHeaderSize = 8;   // update_count, reset_count
    constexpr uint32_t kPathAttHeaderSize = 16;      // path_count, bone_count, vertex_float_count, length_count

    // path 约束的 target 槽位上**没有** PathAttachment。**不是 0** —— 0 是 chunk 20 里
    // 第一条的合法下标，填 0 会让这条约束去摆一条别人的路径，且不报错。
    constexpr uint32_t kNoPathAttachment = 0xFFFFFFFFu;

    // attachment 时间轴里"这一帧没有附件"的哨兵。**不是 0** —— 0 是 chunk 1 里
    // 第一个字符串的合法偏移，填 0 会让空帧变成"挂上某个真实附件"且不报错。
    constexpr uint32_t kNoAttachmentName = 0xFFFFFFFFu;

    // v12：deform 时间轴的 `target` 最高位 = "低 15 位是 chunk 20 的 **path 附件**下标"，
    // 不是附件记录（chunk 7）下标。
    //
    // 为什么 path 的 deform 必须进容器：`VertexAttachment::computeWorldVertices()` 在
    // rigid 分支里**整体替换**、weighted 分支里**逐影响相加**槽位的 deform 数组
    // （VertexAttachment.cpp:69-70 / :117-118）⇒ deform 改了 path 曲线的形状，
    // path 约束摆出来的**骨骼**跟着变。v11 及以前在烘焙期把它当"不渲染、无效果"跳过，
    // 实测 OSGame 75 个逐骨骼对不上的资产里 62 个就是这个原因。
    //
    // 用最高位而不是新时间轴类型：类型值逐个抄参考实现 `TimelineType.h`，不许自造。
    // v15 起次高位 = "低 14 位是 chunk 9 的**裁剪多边形**行号"（裁剪附件的 deform 改的是
    // 裁剪多边形，见 kFormatVersion 的 v15 条）。两位**互斥**（同时置位 = 坏容器）。
    // 代价是附件记录 / path 附件 / 裁剪多边形的下标都必须 < 0x4000（烘焙期显式报错，不截断）。
    constexpr uint16_t kDeformTargetPathBit = 0x8000u;
    constexpr uint16_t kDeformTargetClipBit = 0x4000u;
    constexpr uint16_t kDeformTargetIndexMask = 0x3FFFu;

    // 骨骼数上限。两个约束中取严的：
    //   * `SpineGpuBone::parent` 是 int16 ⇒ 下标必须能用 int16 表示（含 -1）
    //   * `SpineInfluenceBone::bone_idx` 是 uint16（3.8 的 _bones 是 unsigned short）
    // 实测 OSGame 782 个资产远低于此，设它是为了挡"读错段读出一个天文数字"。
    constexpr uint32_t kMaxBones = 32767;
}   // namespace SpineGpuConstants

// ---------------------------------------------------------------------
// 枚举
// ---------------------------------------------------------------------

// 静态 VB 段内的顶点格式。两种并存，不要统一成一种（规格 §3.1）。
enum class SpineVertexFormat : uint8_t
{
    kRigid = 0,     // _bones 为空，整块由槽位骨骼单矩阵变换
    // v7 起名字从 kWeighted4 改成 kWeighted：影响数**不再**是定长 4
    // （枚举值仍是 1，落盘字节不变）。每顶点的影响数写在
    // SpineWeightedVertex::influence_count 里，每个影响有自己的绑定坐标。
    kWeighted = 1,
};

// 与 spine-cpp BlendMode.h:35-38 逐值对应（Normal=0 / Additive / Multiply / Screen）。
enum class SpineBlendMode : uint8_t
{
    kNormal = 0,
    kAdditive = 1,
    kMultiply = 2,
    kScreen = 3,
};

// 逐值抄 spine-cpp `TransformMode.h:34-40`。落盘的就是这个整数
// （不自造一套枚举值 —— 换一套值会让"参考实现的 switch 分支"对不上号）。
//
// 语义见 `Bone.cpp:126-185` 的 `updateWorldTransform()` switch：
// 它决定父骨骼的世界变换里哪些分量传给子骨骼。5 个值**都是实测会出现的**
// （S4 约束阶段要用），不是"理论上存在"。
enum class SpineTransformMode : uint8_t
{
    kNormal = 0,
    kOnlyTranslation = 1,
    kNoRotationOrReflection = 2,
    kNoScale = 3,
    kNoScaleOrReflection = 4,
};

// 绘制项的处置方式。kFallback = 该项顶点由 CPU 算好、走每帧动态 VB。
enum class SpineDrawItemKind : uint8_t
{
    kStatic = 0,    // 可进 GPU 蒙皮路径
    kFallback = 1,  // CPU 路径（裁剪 / FFD 正在动 / 绘制顺序动画等）
};

// 时间轴类型：逐值抄 spine-cpp `TimelineType.h:35-49`。
// ⚠️ **不自造枚举值**：换一套值会让"参考实现的 switch 分支"对不上号，
//    而这只是静默地解错，不会报错。
enum class SpineTimelineType : uint16_t
{
    kRotate = 0,
    kTranslate = 1,
    kScale = 2,
    kShear = 3,
    kAttachment = 4,
    kColor = 5,
    kDeform = 6,
    kEvent = 7,
    kDrawOrder = 8,
    kIkConstraint = 9,
    kTransformConstraint = 10,
    kPathConstraintPosition = 11,
    kPathConstraintSpacing = 12,
    kPathConstraintMix = 13,
    kTwoColor = 14,
};

// 曲线段类型：逐值抄 `CurveTimeline.cpp:42-44`（LINEAR=0 / STEPPED=1 / BEZIER=2）。
enum class SpineCurveType : uint8_t
{
    kLinear = 0,
    kStepped = 1,
    kBezier = 2,
};

// 每帧占几个 float（进 chunk 13 的步长单位）。逐类抄各 Timeline 的 ENTRIES：
//   Rotate 2（`RotateTimeline.h:44`）/ Translate·Scale·Shear 3（`TranslateTimeline.cpp:48`）
//   Color 5（`ColorTimeline.cpp:48`）/ TwoColor 8（`TwoColorTimeline.cpp:48`）
//   IkConstraint 5（`IkConstraintTimeline.cpp:50`）/ TransformConstraint 5（`:50`）
//   PathConstraintPosition·Spacing 2（`PathConstraintPositionTimeline.cpp:50`）
//   PathConstraintMix 3（`PathConstraintMixTimeline.cpp:50`）
// 变长的三类（attachment / deform / drawOrder）只有 **time** 进 chunk 13（1 个），
// 其余部分在 chunk 16 —— 这样"取第 n 帧的时间"对所有类型都是同一个公式。
//
// ⚠️ 单位陷阱：chunk 13 的 `frame_first` 是 **float 下标**不是"第几帧"
//    （各类的每帧 float 数不同，没有统一的帧单位）。写错单位时偏移 0 那条
//    逐字节相同，只有**非 0** 的断言能抓到。
inline constexpr uint32_t SpineTimelineEntries(SpineTimelineType type)
{
    switch (type)
    {
        case SpineTimelineType::kRotate:                  return 2;
        case SpineTimelineType::kTranslate:               return 3;
        case SpineTimelineType::kScale:                   return 3;
        case SpineTimelineType::kShear:                   return 3;
        case SpineTimelineType::kAttachment:              return 1;
        case SpineTimelineType::kColor:                   return 5;
        case SpineTimelineType::kDeform:                  return 1;
        case SpineTimelineType::kEvent:                   return 1;
        case SpineTimelineType::kDrawOrder:               return 1;
        case SpineTimelineType::kIkConstraint:            return 5;
        case SpineTimelineType::kTransformConstraint:     return 5;
        case SpineTimelineType::kPathConstraintPosition:  return 2;
        case SpineTimelineType::kPathConstraintSpacing:   return 2;
        case SpineTimelineType::kPathConstraintMix:       return 3;
        case SpineTimelineType::kTwoColor:                return 8;
    }
    return 0;
}

// 这一类的帧**是否需要曲线**。attachment / drawOrder / event **不是** CurveTimeline
// （`AttachmentTimeline` / `DrawOrderTimeline` / `EventTimeline` 直接继承 Timeline），
// 参考实现根本不给它们读 curve —— 给它们写段会让 chunk 13/14 错位且不报错。
inline constexpr bool SpineTimelineHasCurve(SpineTimelineType type)
{
    return type != SpineTimelineType::kAttachment && type != SpineTimelineType::kDrawOrder &&
           type != SpineTimelineType::kEvent;
}

// Header.flags 位。
enum SpineGpuFlags : uint32_t
{
    kNone = 0u,
    kIndex32 = 1u << 0,               // 索引是 uint32（默认 uint16）
    kHasDrawOrderTimeline = 1u << 1,  // 有绘制顺序时间轴 ⇒ 整个资产走 CPU 路径
    kUsesTintBlack = 1u << 2,         // 用到 tintBlack（dark tint）
    kHasClipping = 1u << 3,           // 有裁剪数据 ⇒ 相关槽位走 CPU fallback
    // 有 deform（FFD）时间轴 ⇒ **被它命中的**绘制项走 CPU fallback（不是整个资产）。
    // per-frame 顶点数据烘不进静态 VB —— 顶点缓冲存的是**绑定姿势**（规格 §7.1）。
    kHasDeform = 1u << 4,
    // 有事件（event）时间轴。事件**不烘焙**（它不改任何渲染状态，且本工程实测 0 个
    // 资产用到），但"有却不记"等于静默丢弃 ⇒ 用一位显式说出来。
    kHasEventTimeline = 1u << 5,
};

// ChunkTable 的 id。
// v3 重新编过号：UV 缓冲插在顶点缓冲后面（id 4），后面整体 +1。
// v2 的 id（strings=1 … clip=8）已经作废 —— 容器版本变了，不做静默兼容。
enum SpineGpuChunkId : uint32_t
{
    kChunkStrings = 1,       // 骨骼名 / 皮肤名 / 槽位名 / 附件名（UTF-8，调试与挂点）
    kChunkSlotBone = 2,      // 每槽位的骨骼下标（u16 × slot_count），rigid 用
    kChunkVertexBuffer = 3,  // 按 vertex_format 分段；**不含 UV**
    kChunkUvBuffer = 4,      // float2 × uv_count，8 B/条；由 DrawItem::uv_offset 索引
    kChunkIndexBuffer = 5,   // uint16（kIndex32 时 uint32）
    kChunkDrawItems = 6,     // SpineDrawItem[]
    kChunkAttachments = 7,   // (皮肤, 槽位) → 附件 id 列表（换装用）
    kChunkAtlasPages = 8,    // 图集页路径 / 纹理引用
    kChunkClipData = 9,      // SpineGpuClipPolygon[]（20 B/条，v15）；**顶点本身在 chunk 3 里**

    // v8：骨骼 setup（36 B/骨）。id 10 就是当初留给它的。
    kChunkBoneSetup = 10,    // SpineGpuBone[]（36 B/条）；下标 = header.bone_count 的序

    // v9：动画（见 §3.2 起的规格）。id 11..16 就是当初留给"v7 动画阶段"的。
    kChunkAnimDir = 11,      // SpineGpuAnim[]（20 B/条）
    kChunkTimelines = 12,    // SpineGpuTimeline[]（32 B/条，全局一张表，动画按区间引用）
    kChunkFrames = 13,       // float[]（帧数据平面数组）
    kChunkCurveTypes = 14,   // uint8[]（每段一个：0 linear / 1 stepped / 2 bezier）
    kChunkBeziers = 15,      // SpineGpuBezier[]（16 B/条，按段序）
    kChunkTimelinePayload = 16,  // 字节（attachment 名 / deform 稀疏帧 / drawOrder 偏移）

    // v10：槽位 setup（40 B/槽）。id 17 就是当初留给它的（10..20 是 v7 动画阶段）。
    kChunkSlotSetup = 17,  // SpineGpuSlot[]（40 B/条）；下标 = header.slot_count 的序

    // v11：约束（S4）。id 18..20 —— 10..20 就是当初留给"v7 动画阶段"的。
    // ⚠️ 为什么必须落盘：约束在 `Skeleton::updateWorldTransform` 里**每帧都跑**，
    //    与有没有时间轴无关（IK 22.5% / transform 48.3% / path 11.1% 的资产有约束）。
    //    只做骨骼时间轴会让近一半资产的姿势静默错（doc/GPU_SPINE_V7_ANIMATION.md §1.3）。
    kChunkConstraintSetup = 18,  // 约束 setup（IK / transform / path + 共用的骨骼索引池）
    kChunkUpdateOrder = 19,      // `Skeleton::updateCache()` 的结果（**直接落结果**）
    kChunkPathAttachments = 20,  // PathAttachment 几何（path 约束要沿它摆骨骼）

    // v7：蒙皮数据流。id 从 21 起 —— 10..20 留给 v7 动画阶段
    // （骨骼树 / 时间轴 / 约束 / path 几何），别占用。
    kChunkInfluenceBind = 21,  // float2 × influence_count（8 B/条）；由 influence_offset 索引
    kChunkInfluenceBone = 22,  // SpineInfluenceBone[]（4 B/条）；与 21 **共用同一个条目下标**
};

// ---------------------------------------------------------------------
// 容器头（64 B，规格 §9）
// ---------------------------------------------------------------------
struct SpineGpuHeader
{
    uint32_t magic;             // 'ZSPG'
    uint32_t version;           // 本规格版本，当前 11（见 kFormatVersion 的版本史）
    uint32_t spine_runtime;     // 产生它的 spine-cpp 版本（3.8.xx → 30800）
    uint32_t flags;             // SpineGpuFlags
    uint32_t bone_count;
    uint32_t slot_count;
    uint32_t draw_item_count;
    uint32_t vertex_count;
    uint32_t index_count;
    uint32_t atlas_page_count;
    uint32_t bone_stride;       // 24 或 32
    uint32_t chunk_count;
    // 0x30..0x3F 原本是"保留"，但**顶点缓冲的分段信息必须进容器**：
    // 光有 vertex_buffer 的字节流，读回来分不出哪段是 rigid、哪段是 weighted
    // （两种格式步长不同），也就无从建立顶点缓冲视图。
    uint32_t rigid_vertex_count;       // 0x30
    uint32_t weighted_segment_offset;  // 0x34 —— 8 B 对齐（Load2 的前提）
    uint32_t weighted_vertex_count;    // 0x38
    uint32_t skin_count;               // 0x3C —— 皮肤名在 chunk 1 里紧接骨骼名之后
};

static_assert(sizeof(SpineGpuHeader) == SpineGpuConstants::kHeaderSize,
              "spec 9: header is 64 B");
static_assert(offsetof(SpineGpuHeader, magic) == 0x00, "spec 9: magic @0x00");
static_assert(offsetof(SpineGpuHeader, version) == 0x04, "spec 9: version @0x04");
static_assert(offsetof(SpineGpuHeader, spine_runtime) == 0x08, "spec 9: spine_runtime @0x08");
static_assert(offsetof(SpineGpuHeader, flags) == 0x0C, "spec 9: flags @0x0C");
static_assert(offsetof(SpineGpuHeader, bone_count) == 0x10, "spec 9: bone_count @0x10");
static_assert(offsetof(SpineGpuHeader, slot_count) == 0x14, "spec 9: slot_count @0x14");
static_assert(offsetof(SpineGpuHeader, draw_item_count) == 0x18, "spec 9: draw_item_count @0x18");
static_assert(offsetof(SpineGpuHeader, vertex_count) == 0x1C, "spec 9: vertex_count @0x1C");
static_assert(offsetof(SpineGpuHeader, index_count) == 0x20, "spec 9: index_count @0x20");
static_assert(offsetof(SpineGpuHeader, atlas_page_count) == 0x24, "spec 9: atlas_page_count @0x24");
static_assert(offsetof(SpineGpuHeader, bone_stride) == 0x28, "spec 9: bone_stride @0x28");
static_assert(offsetof(SpineGpuHeader, chunk_count) == 0x2C, "spec 9: chunk_count @0x2C");
static_assert(offsetof(SpineGpuHeader, rigid_vertex_count) == 0x30, "spec 9: rigid_vertex_count @0x30");
static_assert(offsetof(SpineGpuHeader, weighted_segment_offset) == 0x34,
              "spec 9: weighted_segment_offset @0x34");
static_assert(offsetof(SpineGpuHeader, weighted_vertex_count) == 0x38, "spec 9: weighted_vertex_count @0x38");
static_assert(offsetof(SpineGpuHeader, skin_count) == 0x3C, "spec 9: skin_count @0x3C");

// ChunkTable 项（16 B）。
struct SpineGpuChunk
{
    uint32_t id;      // SpineGpuChunkId
    uint32_t offset;
    uint32_t size;
    uint32_t pad;
};

static_assert(sizeof(SpineGpuChunk) == SpineGpuConstants::kChunkTableEntrySize,
              "spec 9: chunk entry is 16 B");
static_assert(offsetof(SpineGpuChunk, id) == 0x00, "spec 9: chunk.id @0x00");
static_assert(offsetof(SpineGpuChunk, offset) == 0x04, "spec 9: chunk.offset @0x04");
static_assert(offsetof(SpineGpuChunk, size) == 0x08, "spec 9: chunk.size @0x08");

// ---------------------------------------------------------------------
// 骨骼 setup（chunk 10，36 B/骨）
// ---------------------------------------------------------------------
// 逐字段抄 spine-cpp：
//   * 字段集合 `BoneData.h:110-115` —— _index / _name / _parent / _length /
//     _x,_y,_rotation,_scaleX,_scaleY,_shearX,_shearY / _transformMode
//     （_name 不在这里，它在 chunk 1 的字符串表里，按下标取）
//   * `transform_mode` 取值 `TransformMode.h:34-40`（0..4，见下面的枚举）
//
// 为什么必须有它：动画是**相对 setup pose 的增量**。没有 setup 的 x/y/rotation/
// scale/shear，时间轴给的"rotation += 30"就无从合成；槽位的暗色混合、IK 的
// bone length、约束的 offset 也都挂在 setup 上。容器里缺了它，动画阶段（S2 起）
// 就只能再回头读源文件 —— 那容器就不自包含了。
//
// ⚠️ 顺序不变量 **parent < index**（根是 -1）。有它之后：
//   * 正向 for 一遍就是拓扑序 ⇒ `updateWorldTransform` 不需要排序/递归/栈；
//   * 反序列化时**必须校验**：parent 不在这个范围里就说明骨骼表被重排过或读坏了，
//     而重排的后果是"骨骼下标在容器里有了两种含义"，静默错且查不出来。
struct SpineGpuBone
{
    float x = 0.0f;            // @0
    float y = 0.0f;            // @4
    float rotation = 0.0f;     // @8   度，不是弧度（spine 全线用度）
    float scale_x = 1.0f;      // @12  ⚠️ 默认 **1** 不是 0（SkeletonJson.cpp:169-170）
    float scale_y = 1.0f;      // @16
    float shear_x = 0.0f;      // @20
    float shear_y = 0.0f;      // @24
    float length = 0.0f;       // @28  IK / 编辑器挂点用；不画
    int16_t parent = -1;       // @32  -1 = 根；否则必须 **< 自己的下标**
    uint8_t transform_mode = 0;// @34  SpineTransformMode
    uint8_t pad = 0;           // @35  落盘必须写 0
};

static_assert(sizeof(SpineGpuBone) == SpineGpuConstants::kBoneSetupSize,
              "spec 3.1: bone setup record is 36 B");
static_assert(offsetof(SpineGpuBone, x) == 0, "spec 3.1: bone.x @0");
static_assert(offsetof(SpineGpuBone, y) == 4, "spec 3.1: bone.y @4");
static_assert(offsetof(SpineGpuBone, rotation) == 8, "spec 3.1: bone.rotation @8");
static_assert(offsetof(SpineGpuBone, scale_x) == 12, "spec 3.1: bone.scale_x @12");
static_assert(offsetof(SpineGpuBone, scale_y) == 16, "spec 3.1: bone.scale_y @16");
static_assert(offsetof(SpineGpuBone, shear_x) == 20, "spec 3.1: bone.shear_x @20");
static_assert(offsetof(SpineGpuBone, shear_y) == 24, "spec 3.1: bone.shear_y @24");
static_assert(offsetof(SpineGpuBone, length) == 28, "spec 3.1: bone.length @28");
static_assert(offsetof(SpineGpuBone, parent) == 32, "spec 3.1: bone.parent @32");
static_assert(offsetof(SpineGpuBone, transform_mode) == 34, "spec 3.1: bone.transform_mode @34");
static_assert(offsetof(SpineGpuBone, pad) == 35, "spec 3.1: bone.pad @35");
static_assert(std::is_standard_layout_v<SpineGpuBone>,
              "bone setup must stay standard-layout (it is read/written as raw bytes)");
static_assert(alignof(SpineGpuBone) == 4, "bone setup is 4-byte aligned (36 = 9 x u32)");

inline bool operator==(const SpineGpuBone& a, const SpineGpuBone& b)
{
    return a.x == b.x && a.y == b.y && a.rotation == b.rotation && a.scale_x == b.scale_x &&
           a.scale_y == b.scale_y && a.shear_x == b.shear_x && a.shear_y == b.shear_y &&
           a.length == b.length && a.parent == b.parent && a.transform_mode == b.transform_mode &&
           a.pad == b.pad;   // pad 参与比较：落盘必须是 0，放进来才能抓"写端没清零"
}
inline bool operator!=(const SpineGpuBone& a, const SpineGpuBone& b) { return !(a == b); }

// ---------------------------------------------------------------------
// 动画目录（chunk 11，20 B/条）
// ---------------------------------------------------------------------
// 一条 = 一个动画。时间轴是**全局一张表**（chunk 12），动画只持有区间
// `[timeline_first, timeline_first + timeline_count)` ——
// 这样"同一个时间轴被两个动画引用"（Spine 里做不到，但去重后的表不怕）也好表达，
// 且运行时扫描一个动画不需要间接跳转。
//
// ⚠️ `name_offset` 是进 chunk 1 的**字节**偏移（不是"第几个字符串"）：
//    字符串表是变长的，按下标取必须先扫一遍；按偏移取是 O(1)，
//    而且 C# 那边没有 offsetof 之外的捷径，两边都是同一个公式。
//
// ⚠️ 动画**数量不进 header**：header 的 16 个 u32 已经排满，而 chunk 11 的条目
//    是定长 20 B ⇒ 数量 = chunk 11 大小 / 20，读端照此反推并校验整除。
struct SpineGpuAnim
{
    uint32_t name_offset = 0;     // @0   chunk 1 里的字节偏移
    uint32_t timeline_first = 0;  // @4   进 chunk 12 的**条**下标
    uint32_t timeline_count = 0;  // @8
    float duration = 0.0f;        // @12  参考实现 = max(各时间轴最后一帧的 time)
    uint32_t pad = 0;             // @16  落盘必须写 0
};

static_assert(sizeof(SpineGpuAnim) == SpineGpuConstants::kAnimDirSize,
              "spec 3.2: animation directory entry is 20 B");
static_assert(offsetof(SpineGpuAnim, name_offset) == 0, "spec 3.2: anim.name_offset @0");
static_assert(offsetof(SpineGpuAnim, timeline_first) == 4, "spec 3.2: anim.timeline_first @4");
static_assert(offsetof(SpineGpuAnim, timeline_count) == 8, "spec 3.2: anim.timeline_count @8");
static_assert(offsetof(SpineGpuAnim, duration) == 12, "spec 3.2: anim.duration @12");
static_assert(offsetof(SpineGpuAnim, pad) == 16, "spec 3.2: anim.pad @16");
static_assert(std::is_standard_layout_v<SpineGpuAnim>, "anim dir entry must stay standard-layout");

inline bool operator==(const SpineGpuAnim& a, const SpineGpuAnim& b)
{
    return a.name_offset == b.name_offset && a.timeline_first == b.timeline_first &&
           a.timeline_count == b.timeline_count && a.duration == b.duration && a.pad == b.pad;
}
inline bool operator!=(const SpineGpuAnim& a, const SpineGpuAnim& b) { return !(a == b); }

// ---------------------------------------------------------------------
// 时间轴表（chunk 12，32 B/条，全局一张）
// ---------------------------------------------------------------------
// `target` 的含义随 type 变，这也是它叫 target 不叫 bone_index 的原因：
//   * rotate / translate / scale / shear → **骨骼**下标
//   * attachment / color / twoColor      → **槽位**下标
//   * deform                             → **附件记录**（chunk 7 行号）下标
//   * ik / transform / path*             → 对应**约束**下标
//   * drawOrder                          → 恒 0（它作用于整个骨架的槽位顺序）
//
// deform 用"附件记录下标"是有意的：一条记录 = (皮肤, 槽位, 附件) 三元组，
// 正好是 deform 时间轴的寻址键，不需要再给 32 B 塞三个字段。
// v12 起 `target` 最高位置 1 时，低 15 位是 chunk 20 的 **path 附件**下标
// （`kDeformTargetPathBit`）—— path 不产生绘制项，但它的 deform 会改 path 约束摆出来的
// 骨骼位置，不能跳过。boundingbox / clipping / point 仍在烘焙期跳过（不影响任何姿势）。
struct SpineGpuTimeline
{
    uint16_t type = 0;            // @0   SpineTimelineType（TimelineType.h:35-49 原值）
    uint16_t target = 0;          // @2   见上：骨骼 / 槽位 / 附件记录 / 约束 下标
    uint32_t frame_count = 0;     // @4
    // ⚠️ 进 chunk 13 的 **float 下标**（字节 / 4），不是"第几帧"。
    //    每类时间轴每帧的 float 数不同（rotate 2、translate 3、color 5、twoColor 8…），
    //    不存在一个全局统一的"帧"单位 —— 用"帧"去算下标，第一条（偏移 0）看不出错，
    //    第二条起就读到别人的帧数据，且不报错。
    uint32_t frame_first = 0;     // @8
    uint32_t curve_first = 0;     // @12  进 chunk 14 的**段**下标
    uint32_t bezier_first = 0;    // @16  进 chunk 15 的**条**下标
    uint32_t bezier_count = 0;    // @20  这一条时间轴里有几条 bezier
    uint32_t payload_offset = 0;  // @24  进 chunk 16 的**字节**偏移
    uint32_t payload_count = 0;   // @28  载荷里的**条目**数（变长三类是帧数，其余是 0）
};

static_assert(sizeof(SpineGpuTimeline) == SpineGpuConstants::kTimelineEntrySize,
              "spec 3.3: timeline entry is 32 B");
static_assert(offsetof(SpineGpuTimeline, type) == 0, "spec 3.3: timeline.type @0");
static_assert(offsetof(SpineGpuTimeline, target) == 2, "spec 3.3: timeline.target @2");
static_assert(offsetof(SpineGpuTimeline, frame_count) == 4, "spec 3.3: timeline.frame_count @4");
static_assert(offsetof(SpineGpuTimeline, frame_first) == 8, "spec 3.3: timeline.frame_first @8");
static_assert(offsetof(SpineGpuTimeline, curve_first) == 12, "spec 3.3: timeline.curve_first @12");
static_assert(offsetof(SpineGpuTimeline, bezier_first) == 16, "spec 3.3: timeline.bezier_first @16");
static_assert(offsetof(SpineGpuTimeline, bezier_count) == 20, "spec 3.3: timeline.bezier_count @20");
static_assert(offsetof(SpineGpuTimeline, payload_offset) == 24, "spec 3.3: timeline.payload_offset @24");
static_assert(offsetof(SpineGpuTimeline, payload_count) == 28, "spec 3.3: timeline.payload_count @28");
static_assert(std::is_standard_layout_v<SpineGpuTimeline>, "timeline entry must stay standard-layout");

inline bool operator==(const SpineGpuTimeline& a, const SpineGpuTimeline& b)
{
    return a.type == b.type && a.target == b.target && a.frame_count == b.frame_count &&
           a.frame_first == b.frame_first && a.curve_first == b.curve_first &&
           a.bezier_first == b.bezier_first && a.bezier_count == b.bezier_count &&
           a.payload_offset == b.payload_offset && a.payload_count == b.payload_count;
}
inline bool operator!=(const SpineGpuTimeline& a, const SpineGpuTimeline& b) { return !(a == b); }

// ---------------------------------------------------------------------
// bezier 控制点（chunk 15，16 B/条）
// ---------------------------------------------------------------------
// ⚠️ 存**控制点**而不是参考实现那张展开表。`CurveTimeline::setCurve()`（:78-83）
//    把 4 个控制点展开成 `BEZIER_VALUES = 18` 个 float 的采样表；实测 2 567 358 段中
//    bezier 占 36.6%，定长存表要 49.0 MB，存控制点 **16.8 MB**。
//    展开式里的 4 个常数 `0.03f / 0.006f / 0.3f / 0.16666667f`（:80-83）由**读端照抄**，
//    ⚠️ 化简即错且不报错（那是 bezier 的前向差分系数，不是可以重排的算术式）。
struct SpineGpuBezier
{
    float cx1 = 0.0f;   // @0
    float cy1 = 0.0f;   // @4
    float cx2 = 0.0f;   // @8
    float cy2 = 0.0f;   // @12
};

static_assert(sizeof(SpineGpuBezier) == SpineGpuConstants::kBezierSize,
              "spec 3.4: bezier control point record is 16 B");
static_assert(offsetof(SpineGpuBezier, cx1) == 0, "spec 3.4: bezier.cx1 @0");
static_assert(offsetof(SpineGpuBezier, cy1) == 4, "spec 3.4: bezier.cy1 @4");
static_assert(offsetof(SpineGpuBezier, cx2) == 8, "spec 3.4: bezier.cx2 @8");
static_assert(offsetof(SpineGpuBezier, cy2) == 12, "spec 3.4: bezier.cy2 @12");
static_assert(std::is_standard_layout_v<SpineGpuBezier>, "bezier record must stay standard-layout");

inline bool operator==(const SpineGpuBezier& a, const SpineGpuBezier& b)
{
    return a.cx1 == b.cx1 && a.cy1 == b.cy1 && a.cx2 == b.cx2 && a.cy2 == b.cy2;
}
inline bool operator!=(const SpineGpuBezier& a, const SpineGpuBezier& b) { return !(a == b); }

// ---------------------------------------------------------------------
// 槽位 setup（chunk 17，32 B/条）
// ---------------------------------------------------------------------
// 下标 = header.slot_count 的序 = chunk 1 里槽位名的序号 = 时间轴 `target`
// （attachment / color / twoColor）引用的同一个下标空间。
//
// ⚠️ 三处必须是同一个下标空间 —— 分叉了不报错，只是颜色/附件挂到别的槽位上。
//
// 为什么必须进容器：color / twoColor 时间轴写的是**绝对颜色**（`ColorTimeline.cpp`
// 的 apply 直接把插值结果写进 `slot._color`），而"没有时间轴命中"时槽位必须回到
// **setup 色**（`Slot::setToSetupPose()` 就是 `SlotData::getColor()` 拷过来）。
// 容器里只有 `kUsesTintBlack` 一个布尔、没有色值 ⇒ 解算器会把"没被动画"的槽位
// 默认成纯白，而纯白和 setup 白在**不写 dark 的资产上完全看不出区别** ——
// 正是那种"错了但不报错"的坑。
//
// ⚠️ dark 全 0 **不等于**"没有暗色"：参考实现里 dark 的默认值就是 (0,0,0,0)
// （`SlotData.cpp:40`），而 3.7 的暗色判据是"读到的 int32 != -1"，
// 0x00000000 照样 != -1 ⇒ **黑也是一种合法的暗色**。
// 所以"有没有暗色"这件事由 `kUsesTintBlack` + 逐槽位的 `has_dark` 表达，
// 不能靠 dark 是否全 0 去推。
// 逐槽位 flags（chunk 17 @36）。
enum SpineGpuSlotFlags : uint32_t
{
    kSlotFlagNone = 0u,
    // 这个槽位**有暗色**（tint black）。渲染要不要走双色分支看的是它，
    // 不是"dark 是否全 0" —— 3.7 的判据是 `读到的 int32 != -1`，0x00000000（纯黑）
    // 照样是"有暗色"（`SkeletonBinary` 3.7 :190-192）。
    kSlotFlagHasDark = 1u << 0,
};

struct SpineGpuSlot
{
    float color_r = 1.0f;   // @0
    float color_g = 1.0f;   // @4
    float color_b = 1.0f;   // @8
    float color_a = 1.0f;   // @12
    float dark_r = 0.0f;    // @16
    float dark_g = 0.0f;    // @20
    float dark_b = 0.0f;    // @24
    float dark_a = 0.0f;    // @28  **有暗色时恒 1**（参考实现写死的，见上）
    // setup 附件名在 chunk 1 里的**字节**偏移；`kNoAttachmentName` = 空名
    // （这个槽位默认不挂附件）。attachment 时间轴在 `time < frames[0]` 时复位到它
    // （`AttachmentTimeline.cpp:65-68`）。
    uint32_t setup_attachment_name_offset = SpineGpuConstants::kNoAttachmentName;  // @32
    uint32_t flags = 0;      // @36  SpineGpuSlotFlags
};

static_assert(sizeof(SpineGpuSlot) == SpineGpuConstants::kSlotSetupSize,
              "spec 3.5: slot setup record is 40 B");
static_assert(offsetof(SpineGpuSlot, color_r) == 0, "spec 3.5: slot.color_r @0");
static_assert(offsetof(SpineGpuSlot, color_g) == 4, "spec 3.5: slot.color_g @4");
static_assert(offsetof(SpineGpuSlot, color_b) == 8, "spec 3.5: slot.color_b @8");
static_assert(offsetof(SpineGpuSlot, color_a) == 12, "spec 3.5: slot.color_a @12");
static_assert(offsetof(SpineGpuSlot, dark_r) == 16, "spec 3.5: slot.dark_r @16");
static_assert(offsetof(SpineGpuSlot, dark_g) == 20, "spec 3.5: slot.dark_g @20");
static_assert(offsetof(SpineGpuSlot, dark_b) == 24, "spec 3.5: slot.dark_b @24");
static_assert(offsetof(SpineGpuSlot, dark_a) == 28, "spec 3.5: slot.dark_a @28");
static_assert(offsetof(SpineGpuSlot, setup_attachment_name_offset) == 32,
              "spec 3.5: slot.setup_attachment_name_offset @32");
static_assert(offsetof(SpineGpuSlot, flags) == 36, "spec 3.5: slot.flags @36");
static_assert(std::is_standard_layout_v<SpineGpuSlot>, "slot setup must stay standard-layout");
static_assert(alignof(SpineGpuSlot) == 4, "slot setup is 4-byte aligned (40 = 10 x u32)");

inline bool operator==(const SpineGpuSlot& a, const SpineGpuSlot& b)
{
    return a.color_r == b.color_r && a.color_g == b.color_g && a.color_b == b.color_b &&
           a.color_a == b.color_a && a.dark_r == b.dark_r && a.dark_g == b.dark_g &&
           a.dark_b == b.dark_b && a.dark_a == b.dark_a &&
           a.setup_attachment_name_offset == b.setup_attachment_name_offset && a.flags == b.flags;
}
inline bool operator!=(const SpineGpuSlot& a, const SpineGpuSlot& b) { return !(a == b); }

// ---------------------------------------------------------------------
// 约束（chunk 18，v11）
// ---------------------------------------------------------------------
// 为什么必须进容器：约束在 `Skeleton::updateWorldTransform()` 里**每帧都跑**，
// 与"这条约束有没有时间轴"无关 —— 时间轴改的是 mix / position 这些**参数**，
// 约束本身的存在性与参数由 setup 决定。只烘焙骨骼时间轴会让近一半资产的
// 姿势静默错（IK 22.5% / transform 48.3% / path 11.1%，见 v7 文档 §1.3）。
//
// ⚠️ 三条约束的 `target` **不是同一个下标空间**：
//    IK / transform 的 target 是**骨骼**下标，path 的 target 是**槽位**下标
//    （`PathConstraintData::_target` 是 `SlotData*`）。写反了不报错，
//    只是约束一直作用在错误的对象上。
//
// 字段集合逐条抄 3.7 `SkeletonBinary.cpp:190-257` / `SkeletonJson.cpp:255-422`；
// 两份输入的默认值不一样（json 有默认、二进制全是显式写入），
// 由"skel ↔ json Bake 后逐字节相同"这条对拍钉住。
//
// `bones[]` 长度可变 ⇒ 三类**共用** chunk 18 尾部的一段 u16 索引池，
// 每条约束存 `bone_first` + `bone_count`（与 v7 影响流同一个思路）。

// IK（20 B/条），字段抄 `IkConstraintData.h` + `SkeletonBinary.cpp:190-208`
struct SpineGpuIkConstraint
{
    uint16_t order = 0;        // @0  三类约束**共用**一个 order 排名空间（见 chunk 19）
    uint16_t bone_count = 0;   // @2
    uint32_t bone_first = 0;   // @4  进 chunk 18 的 u16 骨骼索引池
    uint32_t target = 0;       // @8  目标**骨骼**下标
    float mix = 1.0f;          // @12 json 默认 1（SkeletonJson.cpp:290）
    int16_t bend_dir = 1;      // @16 1 或 -1；json 是 `bendPositive` 布尔（:291）
    uint16_t flags = 0;        // @18 见 kIkFlag*
};

// transform（56 B/条），字段抄 `TransformConstraintData.h` + `SkeletonBinary.cpp:217-237`
struct SpineGpuTransformConstraint
{
    uint16_t order = 0;        // @0
    uint16_t bone_count = 0;   // @2
    uint32_t bone_first = 0;   // @4
    uint32_t target = 0;       // @8  目标**骨骼**下标
    float offset_rotation = 0.0f;  // @12
    float offset_x = 0.0f;         // @16
    float offset_y = 0.0f;         // @20
    float offset_scale_x = 0.0f;   // @24
    float offset_scale_y = 0.0f;   // @28
    float offset_shear_y = 0.0f;   // @32
    float rotate_mix = 1.0f;       // @36
    float translate_mix = 1.0f;    // @40
    float scale_mix = 1.0f;        // @44
    float shear_mix = 1.0f;        // @48
    uint16_t flags = 0;            // @52 见 kTfFlag*
    uint16_t pad = 0;              // @54 落盘必须写 0
};

// path（40 B/条），字段抄 `PathConstraintData.h` + `SkeletonBinary.cpp:239-257`
struct SpineGpuPathConstraint
{
    uint16_t order = 0;        // @0
    uint16_t bone_count = 0;   // @2
    uint32_t bone_first = 0;   // @4
    // ⚠️ 是**槽位**下标不是骨骼（`SkeletonBinary.cpp:249` 取的是 `_slots[...]`）
    uint32_t target_slot = 0;  // @8
    uint8_t position_mode = 0; // @12 kPathPositionMode*
    uint8_t spacing_mode = 0;  // @13 kPathSpacingMode*
    uint8_t rotate_mode = 0;   // @14 kPathRotateMode*
    uint8_t pad = 0;           // @15 落盘必须写 0
    float offset_rotation = 0.0f;  // @16
    // ⚠️ `position` / `spacing` 是否乘 scale 取决于 mode（Fixed / Length）—— 那是
    //    **解析期**的事（`SkeletonBinary.cpp:252-255`），运行时不要再乘一次。
    float position = 0.0f;         // @20
    float spacing = 0.0f;          // @24
    float rotate_mix = 1.0f;       // @28
    float translate_mix = 1.0f;    // @32
    // target 槽位上的 PathAttachment 在 chunk 20 里的下标；
    // kNoPathAttachment = "这个槽位上没有 path 附件"（参考实现 `sortPathConstraint`
    // 会退化成 `sortBone(slot.getBone())`）。
    uint32_t path_index = SpineGpuConstants::kNoPathAttachment;  // @36
};

enum SpineGpuIkFlags : uint16_t
{
    kIkFlagNone = 0u,
    kIkFlagCompress = 1u << 0,
    kIkFlagStretch = 1u << 1,
    kIkFlagUniform = 1u << 2,
};

enum SpineGpuTransformFlags : uint16_t
{
    kTfFlagNone = 0u,
    kTfFlagLocal = 1u << 0,
    kTfFlagRelative = 1u << 1,
};

// 逐值抄 3.7 `PathConstraintData.h:39-43`（PositionMode / SpacingMode / RotateMode）
enum SpineGpuPathPositionMode : uint8_t { kPathPosFixed = 0, kPathPosPercent = 1 };
enum SpineGpuPathSpacingMode : uint8_t
{
    kPathSpacingLength = 0, kPathSpacingFixed = 1, kPathSpacingPercent = 2,
};
enum SpineGpuPathRotateMode : uint8_t
{
    kPathRotateTangent = 0, kPathRotateChain = 1, kPathRotateChainScale = 2,
};

static_assert(sizeof(SpineGpuIkConstraint) == SpineGpuConstants::kIkConstraintSize,
              "spec: ik constraint is 20 B");
static_assert(offsetof(SpineGpuIkConstraint, order) == 0, "ik.order @0");
static_assert(offsetof(SpineGpuIkConstraint, bone_count) == 2, "ik.bone_count @2");
static_assert(offsetof(SpineGpuIkConstraint, bone_first) == 4, "ik.bone_first @4");
static_assert(offsetof(SpineGpuIkConstraint, target) == 8, "ik.target @8");
static_assert(offsetof(SpineGpuIkConstraint, mix) == 12, "ik.mix @12");
static_assert(offsetof(SpineGpuIkConstraint, bend_dir) == 16, "ik.bend_dir @16");
static_assert(offsetof(SpineGpuIkConstraint, flags) == 18, "ik.flags @18");

static_assert(sizeof(SpineGpuTransformConstraint) == SpineGpuConstants::kTransformConstraintSize,
              "spec: transform constraint is 56 B");
static_assert(offsetof(SpineGpuTransformConstraint, target) == 8, "tf.target @8");
static_assert(offsetof(SpineGpuTransformConstraint, offset_rotation) == 12, "tf.offset_rotation @12");
static_assert(offsetof(SpineGpuTransformConstraint, offset_shear_y) == 32, "tf.offset_shear_y @32");
static_assert(offsetof(SpineGpuTransformConstraint, shear_mix) == 48, "tf.shear_mix @48");
static_assert(offsetof(SpineGpuTransformConstraint, flags) == 52, "tf.flags @52");
static_assert(offsetof(SpineGpuTransformConstraint, pad) == 54, "tf.pad @54");

static_assert(sizeof(SpineGpuPathConstraint) == SpineGpuConstants::kPathConstraintSize,
              "spec: path constraint is 40 B");
static_assert(offsetof(SpineGpuPathConstraint, target_slot) == 8, "path.target_slot @8");
static_assert(offsetof(SpineGpuPathConstraint, position_mode) == 12, "path.position_mode @12");
static_assert(offsetof(SpineGpuPathConstraint, spacing_mode) == 13, "path.spacing_mode @13");
static_assert(offsetof(SpineGpuPathConstraint, rotate_mode) == 14, "path.rotate_mode @14");
static_assert(offsetof(SpineGpuPathConstraint, pad) == 15, "path.pad @15");
static_assert(offsetof(SpineGpuPathConstraint, offset_rotation) == 16, "path.offset_rotation @16");
static_assert(offsetof(SpineGpuPathConstraint, translate_mix) == 32, "path.translate_mix @32");
static_assert(offsetof(SpineGpuPathConstraint, path_index) == 36, "path.path_index @36");

static_assert(std::is_standard_layout_v<SpineGpuIkConstraint>, "ik must stay standard-layout");
static_assert(std::is_standard_layout_v<SpineGpuTransformConstraint>, "tf must stay standard-layout");
static_assert(std::is_standard_layout_v<SpineGpuPathConstraint>, "path must stay standard-layout");

inline bool operator==(const SpineGpuIkConstraint& a, const SpineGpuIkConstraint& b)
{
    return a.order == b.order && a.bone_count == b.bone_count && a.bone_first == b.bone_first &&
           a.target == b.target && a.mix == b.mix && a.bend_dir == b.bend_dir && a.flags == b.flags;
}
inline bool operator!=(const SpineGpuIkConstraint& a, const SpineGpuIkConstraint& b)
{
    return !(a == b);
}

inline bool operator==(const SpineGpuTransformConstraint& a, const SpineGpuTransformConstraint& b)
{
    return a.order == b.order && a.bone_count == b.bone_count && a.bone_first == b.bone_first &&
           a.target == b.target && a.offset_rotation == b.offset_rotation && a.offset_x == b.offset_x &&
           a.offset_y == b.offset_y && a.offset_scale_x == b.offset_scale_x &&
           a.offset_scale_y == b.offset_scale_y && a.offset_shear_y == b.offset_shear_y &&
           a.rotate_mix == b.rotate_mix && a.translate_mix == b.translate_mix &&
           a.scale_mix == b.scale_mix && a.shear_mix == b.shear_mix && a.flags == b.flags &&
           a.pad == b.pad;
}
inline bool operator!=(const SpineGpuTransformConstraint& a, const SpineGpuTransformConstraint& b)
{
    return !(a == b);
}

inline bool operator==(const SpineGpuPathConstraint& a, const SpineGpuPathConstraint& b)
{
    return a.order == b.order && a.bone_count == b.bone_count && a.bone_first == b.bone_first &&
           a.target_slot == b.target_slot && a.position_mode == b.position_mode &&
           a.spacing_mode == b.spacing_mode && a.rotate_mode == b.rotate_mode && a.pad == b.pad &&
           a.offset_rotation == b.offset_rotation && a.position == b.position &&
           a.spacing == b.spacing && a.rotate_mix == b.rotate_mix &&
           a.translate_mix == b.translate_mix && a.path_index == b.path_index;
}
inline bool operator!=(const SpineGpuPathConstraint& a, const SpineGpuPathConstraint& b)
{
    return !(a == b);
}

// ---------------------------------------------------------------------
// 执行顺序（chunk 19，4 B/条，v11）
// ---------------------------------------------------------------------
// `Skeleton::updateCache()`（3.7 `Skeleton.cpp:134-178`）算出来的 `_updateCache`
// **原样落盘**，运行时照着遍历就行。
//
// 为什么不落"让运行时重排"：重排规则有 `sortBone` 的递归 + 约束 order 两层
// （`order` 是**三类约束共用的排名**，且 `updateCache` 用 `goto continue_outer`
//  按 i = 0..N-1 逐个找 order == i 的那条）。运行时再实现一遍就是给自己
// 多一次写反的机会，而写反的表现是"姿势略偏"，查不出来。
//
// ⚠️ `_updateCacheReset` 也要落：它是"在遍历之前要把哪些骨骼复位到 setup"
// 的列表（3.7 `Skeleton.cpp:200-211`）。少复位会让上一帧的应用值残留，
// 表现是"动作拖影"，同样不报错。
enum SpineGpuUpdateKind : uint8_t
{
    kUpdateBone = 0,
    kUpdateIk = 1,
    kUpdateTransform = 2,
    kUpdatePath = 3,
};

struct SpineGpuUpdateStep
{
    uint8_t kind = 0;       // @0  SpineGpuUpdateKind
    uint8_t pad = 0;        // @1  落盘必须写 0
    // 约束下标是**该类内的**下标（ik / transform / path 各自从 0 数），
    // 与 chunk 18 三段数组的下标一致。
    uint16_t index = 0;     // @2
};

static_assert(sizeof(SpineGpuUpdateStep) == SpineGpuConstants::kUpdateStepSize,
              "spec: update step is 4 B");
static_assert(offsetof(SpineGpuUpdateStep, kind) == 0, "step.kind @0");
static_assert(offsetof(SpineGpuUpdateStep, pad) == 1, "step.pad @1");
static_assert(offsetof(SpineGpuUpdateStep, index) == 2, "step.index @2");
static_assert(std::is_standard_layout_v<SpineGpuUpdateStep>, "step must stay standard-layout");

inline bool operator==(const SpineGpuUpdateStep& a, const SpineGpuUpdateStep& b)
{
    return a.kind == b.kind && a.pad == b.pad && a.index == b.index;
}
inline bool operator!=(const SpineGpuUpdateStep& a, const SpineGpuUpdateStep& b)
{
    return !(a == b);
}

// ---------------------------------------------------------------------
// PathAttachment 几何（chunk 20，36 B/条，v11）
// ---------------------------------------------------------------------
// path 约束的 target 是**槽位**，槽位上挂的 PathAttachment 才是骨骼要摆的曲线。
// 没有它就算不出 `PathConstraint::computeWorldPositions()` ⇒ 必须落盘。
//
// ⚠️ 它**不产生绘制项**（与 boundingbox / point / clipping 一样），
//    但仍然要烘焙 —— 这是 v11 把 path 从"不烘焙"升级成"烘焙但不画"的原因
//    （v10 及以前 path 只在 `no_draw_attachments` 里留个名字）。
//
// 编码与 mesh 顶点**同构**（`SkeletonBinary::readVertices`，:1108-1130）：
//   rigid    → vertices = [x, y] × vertex_count，bones 为空
//   weighted → bones = [n, idx × n] × vertex_count（游程拼接），
//              vertices = [x, y, w] × Σn
// ⇒ `weighted` 必须显式存一个 bit：bones 为空既可能是 rigid，也可能是数据坏。
struct SpineGpuPathAttachment
{
    uint16_t slot_index = 0;      // @0
    // ⚠️ 皮肤**必须**落盘：`PathConstraint::update()` 用的是
    //    `_target->getAttachment()`（PathConstraint.cpp:77）—— 槽位**当前**挂的附件，
    //    而"当前"取决于激活的皮肤。只按槽位找会在换装后拿到别的皮肤的曲线。
    uint16_t skin_index = 0;      // @2
    uint16_t flags = 0;           // @4  kPathAttFlag*
    uint16_t pad = 0;             // @6  落盘必须写 0
    uint32_t vertex_count = 0;    // @8
    uint32_t bone_offset = 0;     // @12 进 chunk 20 的 u16 池（**下标**不是字节）
    uint32_t bone_count = 0;      // @16 u16 池条目数（rigid = 0）
    uint32_t vertex_offset = 0;   // @20 进 chunk 20 的 f32 池（**下标**不是字节）
    uint32_t vertex_float_count = 0;  // @24
    uint32_t length_offset = 0;   // @28 进 lengths 池（f32 下标）
    uint32_t length_count = 0;    // @32  = vertex_count / 3（SkeletonBinary.cpp:550）
    // ⚠️ 附件名**必须**落盘：`PathConstraint::update()` 用的是
    //    `_target->getAttachment()`（PathConstraint.cpp:76-80）—— 槽位**当前**挂的附件，
    //    而"当前"会被 attachment 时间轴改掉。一个槽位在同一皮肤下挂两条 path
    //    附件是**合法的**（实测 782 个资产里有 6 个，如 `K_Lubandashi_Spine` 的
    //    槽位 `EFFTAIYILIZI` 挂了 `EFFTAIYILIZI` 与 `EFFTAIYILIZI2`），
    //    ⇒ 静态烘焙只能给出"setup 时用的是哪条"（`path_index`），
    //      运行时在 attachment 时间轴命中后必须**按名字重新查**。
    uint32_t name_offset = 0;     // @36 进字符串表第 9 组
};

enum SpineGpuPathAttachmentFlags : uint16_t
{
    kPathAttFlagNone = 0u,
    kPathAttFlagClosed = 1u << 0,
    kPathAttFlagConstantSpeed = 1u << 1,
    kPathAttFlagWeighted = 1u << 2,
};

static_assert(sizeof(SpineGpuPathAttachment) == SpineGpuConstants::kPathAttachmentSize,
              "spec: path attachment record is 40 B");
static_assert(offsetof(SpineGpuPathAttachment, slot_index) == 0, "path_att.slot_index @0");
static_assert(offsetof(SpineGpuPathAttachment, skin_index) == 2, "path_att.skin_index @2");
static_assert(offsetof(SpineGpuPathAttachment, flags) == 4, "path_att.flags @4");
static_assert(offsetof(SpineGpuPathAttachment, pad) == 6, "path_att.pad @6");
static_assert(offsetof(SpineGpuPathAttachment, vertex_count) == 8, "path_att.vertex_count @8");
static_assert(offsetof(SpineGpuPathAttachment, bone_offset) == 12, "path_att.bone_offset @12");
static_assert(offsetof(SpineGpuPathAttachment, bone_count) == 16, "path_att.bone_count @16");
static_assert(offsetof(SpineGpuPathAttachment, vertex_offset) == 20, "path_att.vertex_offset @20");
static_assert(offsetof(SpineGpuPathAttachment, vertex_float_count) == 24, "path_att.vertex_float_count @24");
static_assert(offsetof(SpineGpuPathAttachment, length_offset) == 28, "path_att.length_offset @28");
static_assert(offsetof(SpineGpuPathAttachment, length_count) == 32, "path_att.length_count @32");
static_assert(offsetof(SpineGpuPathAttachment, name_offset) == 36, "path_att.name_offset @36");
static_assert(std::is_standard_layout_v<SpineGpuPathAttachment>, "path_att stays standard-layout");

// 一条 path 附件的 deform 数组长度（float 个数）。抄 `SkeletonBinary.cpp:976-977`：
//   weighted ? vertices.size() / 3 * 2 : vertices.size()
// weighted 的 `vertices` 是每个影响 (x, y, weight) 3 个 float，deform 是每个影响 (dx, dy)
// 2 个 ⇒ 不是"顶点数 × 2"。用错时 rigid（偏移 0、长度相同）测不出来，只有 weighted 能。
inline constexpr uint32_t SpineGpuPathDeformLength(const SpineGpuPathAttachment& a)
{
    return a.bone_count == 0 ? a.vertex_float_count : a.vertex_float_count / 3u * 2u;
}

inline bool operator==(const SpineGpuPathAttachment& a, const SpineGpuPathAttachment& b)
{
    return a.slot_index == b.slot_index && a.skin_index == b.skin_index && a.flags == b.flags &&
           a.pad == b.pad && a.vertex_count == b.vertex_count &&
           a.bone_offset == b.bone_offset && a.bone_count == b.bone_count &&
           a.vertex_offset == b.vertex_offset && a.vertex_float_count == b.vertex_float_count &&
           a.length_offset == b.length_offset && a.length_count == b.length_count &&
           a.name_offset == b.name_offset;
}
inline bool operator!=(const SpineGpuPathAttachment& a, const SpineGpuPathAttachment& b)
{
    return !(a == b);
}

// ---------------------------------------------------------------------
// 附件记录（chunk 7，规格 §9）
// ---------------------------------------------------------------------
// 一条 = 一个 (皮肤, 槽位) 下的一个附件。换装查的就是这张表。
//
// v1 只有 (slot_index, attachment_id)：那等于把所有 skin 合并成一份，
// 同名跨 skin 的附件会互相覆盖 —— 而"skin2 的眼睛复用 default 的眼睛"
// 恰好是 linked mesh 最常见的用法，一合并就丢。所以 v2 加了 skin_id 这一列。
//
// attachment_id 是全局下标（= 本表的行号，也 = chunk 1 里附件名的序号）。
// 附件**名**在 chunk 1 里按 attachment_id 顺序排，所以这里不用存名字偏移。
struct SpineGpuAttachmentRecord
{
    uint16_t slot_index = 0;
    uint16_t attachment_id = 0;
    uint16_t skin_id = 0;
    uint16_t pad = 0;   // 凑 8 B，保持 4 B 对齐；落盘时必须写 0
};

static_assert(sizeof(SpineGpuAttachmentRecord) == SpineGpuConstants::kAttachmentRecordSize,
              "spec 9: attachment record is 8 B (v2: +skin column)");
static_assert(offsetof(SpineGpuAttachmentRecord, slot_index) == 0, "spec 9: record.slot_index @0");
static_assert(offsetof(SpineGpuAttachmentRecord, attachment_id) == 2, "spec 9: record.attachment_id @2");
static_assert(offsetof(SpineGpuAttachmentRecord, skin_id) == 4, "spec 9: record.skin_id @4");
static_assert(offsetof(SpineGpuAttachmentRecord, pad) == 6, "spec 9: record.pad @6");

// ---------------------------------------------------------------------
// 静态顶点缓冲（规格 §3.1）
// ---------------------------------------------------------------------
// ⚠️ **UV 不在这里**（v3 起）。顶点只存"几何 + 蒙皮"，
//    UV 在独立的 chunk 4 里，由 DrawItem::uv_offset 索引。
//    拆开是为了让**几何相同的附件共用同一段顶点** —— linked mesh 继承父附件的
//    顶点却用自己的图集 region 算 UV；换装变体也常常只换贴图不改网格。
//    UV 编在顶点里时这两类都只能整份复制（规格 §14.2）。
//    代价：shader 里多一次间接读取（先取索引、再取 UV）。

// rigid：bindPos 2 float。整块由槽位骨骼单矩阵变换。
struct SpineRigidVertex
{
    float bind_pos[2];
};

static_assert(sizeof(SpineRigidVertex) == SpineGpuConstants::kRigidVertexStride,
              "spec 3.1: kVtxRigid is 8 B (uv moved out)");
static_assert(offsetof(SpineRigidVertex, bind_pos) == 0, "spec 3.1: bindPos @0");
// bindPos 必须在最前：Load2 要求 8 字节对齐，步长 8 时每个顶点都在 8 的倍数上。
static_assert(SpineGpuConstants::kRigidVertexStride % 8 == 0,
              "rigid stride must keep bindPos 8-byte aligned");

// weighted：每个影响**一份** bindPos（不是每顶点一份）+ uint16 骨骼索引 + unorm8 权重。
// weighted 顶点（v7 起）：**只是一条指向影响流的引用**，真正的蒙皮数据在
// chunk 21（bindPos）与 chunk 22（boneIdx+weight）里。
//
//   influence_offset : 影响流的**条目**下标（不是字节）—— 两条流共用同一个下标
//   influence_count  : 这个顶点有几个影响（>= 1）
//
// 蒙皮循环（shader）就变成：
//   for (i = 0; i < influence_count; ++i) {
//       float2 bp = bind.Load2((influence_offset + i) * 8);
//       uint   bw = bone.Load ((influence_offset + i) * 4);   // boneIdx u16 | weight u8 << 16
//       ...
//   }
// 均值 2.20 次迭代，而不是定长的 4 次（OSGame 实测）。
struct SpineWeightedVertex
{
    uint32_t influence_offset;   // @0
    uint32_t influence_count;    // @4
};

static_assert(sizeof(SpineWeightedVertex) == SpineGpuConstants::kWeightedVertexStride,
              "spec 3.1: weighted vertex is 8 B (offset + count)");
static_assert(offsetof(SpineWeightedVertex, influence_offset) == 0, "spec 3.1: influence_offset @0");
static_assert(offsetof(SpineWeightedVertex, influence_count) == 4, "spec 3.1: influence_count @4");
static_assert(alignof(SpineWeightedVertex) == 4, "weighted vertex must stay 4-byte aligned");
static_assert(SpineGpuConstants::kWeightedVertexStride % 8 == 0,
              "weighted stride must be 8-byte aligned (spec 3.1)");

// 影响流里的一条骨骼+权重（chunk 22，4 B）。
// ⚠️ 用 u32 一次 Load 出来再拆，而不是 Load u16 + Load u8：后者第二次 Load 的地址
//    不是 4 字节对齐的（boneIdx 在 @0、weight 在 @2）⇒ 越界/未定义。
struct SpineInfluenceBone
{
    uint16_t bone_idx;   // @0
    uint8_t weight;      // @2，unorm8
    uint8_t pad;         // @3
};

static_assert(sizeof(SpineInfluenceBone) == SpineGpuConstants::kInfluenceBoneStride,
              "spec 3.1: influence bone record is 4 B");
static_assert(offsetof(SpineInfluenceBone, bone_idx) == 0, "spec 3.1: boneIdx @0");
static_assert(offsetof(SpineInfluenceBone, weight) == 2, "spec 3.1: weight @2");
// 把「3.8 ⇒ 骨骼索引是 uint16」钉成编译期断言。
static_assert(std::is_same_v<decltype(SpineInfluenceBone::bone_idx), uint16_t>,
              "Spine 3.8: _bones is Vector<unsigned short> -> bone index must be uint16 (spec 1.1)");

// pad 也参与比较：它落盘必须是 0（跟裁剪多边形同一个规矩），
// 放进去才能抓到"写端没清零"这种不报错的脏字节。
inline bool operator==(const SpineInfluenceBone& a, const SpineInfluenceBone& b)
{
    return a.bone_idx == b.bone_idx && a.weight == b.weight && a.pad == b.pad;
}
inline bool operator!=(const SpineInfluenceBone& a, const SpineInfluenceBone& b) { return !(a == b); }

// ---------------------------------------------------------------------
// 绘制项标志（`SpineDrawItem::item_flags`，v15）
namespace SpineDrawItemFlags
{
    // 附件是 RegionAttachment（不是 mesh）。容器里 region 的 4 个顶点按 `_vertexOffset` 的
    // BL, UL, UR, BR 排、三角形 {0,1,2, 2,3,0}；参考实现喂给裁剪器的是 computeWorldVertices 的
    // BR, BL, UL, UR + 同一组三角形 ⇒ CPU 裁剪时要按 {3, 0, 1, 2} 重排（SpineGpuClipper）。
    constexpr uint8_t kRegion = 1u << 0;
    constexpr uint8_t kKnownMask = kRegion;
}   // namespace SpineDrawItemFlags

// 绘制项表（静态，规格 §4）
// ---------------------------------------------------------------------
// 合批键 = (atlas_page, blend_mode, vertex_format, kind)。
struct SpineDrawItem
{
    uint32_t index_offset;
    uint32_t index_count;
    // ⚠️ **UV 条目的下标，不是字节偏移**（chunk 4 里一条 8 B）。
    // 顶点可以被多个绘制项共用（几何相同），但 UV 各取各的 ⇒ 必须有这一项。
    // ⚠️ v13 起是"偏置"：第 q 个索引的 UV 是 `uv_buffer[uv_offset + index_buffer[q]]`
    //    （uint32 回绕加法），因为索引是段内**绝对**下标、UV 按附件本地下标排。
    uint32_t uv_offset;                // @8
    SpineVertexFormat vertex_format;   // @12
    SpineBlendMode blend_mode;         // @13
    uint16_t atlas_page;               // @14
    uint16_t slot_index;               // @16
    uint16_t attachment_id;            // @18
    // @20 —— 这个绘制项属于哪个皮肤。**没有它就没法换装**：
    // 同一个 (槽位, 附件名) 在不同皮肤下是两个绘制项，
    // 光靠 slot_index + attachment_id 分不出该画哪一个。
    uint16_t skin_id;
    SpineDrawItemKind kind;            // @22
    // @23（v15）—— SpineDrawItemFlags（v14 及以前是写 0 的 pad）。
    uint8_t item_flags;
    // @24 —— 这一项被哪条裁剪多边形裁（chunk 9 的行号）。**不被裁时是 kNoClipIndex**。
    // 为什么要存下标而不是让运行时按 (skin_id, slot_index) 去查：一个槽位在**静态上**
    // 可能被多条裁剪覆盖过（不同皮肤 / 嵌套），绘制项要的是"当时生效的那一条"，
    // 而这个只有导入期模拟状态机才知道。
    uint16_t clip_index;
    // @26（v14）—— 这一项的顶点在所在顶点段里的起点（索引 = 附件本地下标 + vertex_base）。
    // 索引是 u16 ⇒ 它必然 ≤ 0xFFFF（烘焙期校验，不截断）。GPU deform 靠它把段内绝对
    // 下标换回附件本地下标（deform 数组按本地下标排）。
    uint16_t vertex_base;
    // @28（v14）—— linked mesh 且 inheritDeform 时 = 父附件的 attachment_id，
    // 否则 `kNoDeformParent`。deform 时间轴作用于本项 ⇔ 时间轴的附件是自己或这个父附件
    // （MeshAttachment.cpp:94-95，只看**一层**父，不沿链传播）。
    uint16_t deform_parent;
    // @30（v14）—— 附件的顶点数（段内 [vertex_base, vertex_base + vertex_count)）。
    // deform 数组长度由它定：rigid = 2 × vertex_count，weighted = 2 × 这些顶点的影响数之和。
    uint16_t vertex_count;
};

// v14 起是 **32 B**。序列化时两端都必须显式把未用字节写 0，否则 C# 与 C++ 会对不上。
static_assert(sizeof(SpineDrawItem) == SpineGpuConstants::kDrawItemSize,
              "spec 4: draw item serializes as 32 B (v14)");
static_assert(offsetof(SpineDrawItem, index_offset) == 0, "spec 4: index_offset @0");
static_assert(offsetof(SpineDrawItem, index_count) == 4, "spec 4: index_count @4");
static_assert(offsetof(SpineDrawItem, uv_offset) == 8, "spec 4: uv_offset @8");
static_assert(offsetof(SpineDrawItem, vertex_format) == 12, "spec 4: vertex_format @12");
static_assert(offsetof(SpineDrawItem, blend_mode) == 13, "spec 4: blend_mode @13");
static_assert(offsetof(SpineDrawItem, atlas_page) == 14, "spec 4: atlas_page @14");
static_assert(offsetof(SpineDrawItem, slot_index) == 16, "spec 4: slot_index @16");
static_assert(offsetof(SpineDrawItem, attachment_id) == 18, "spec 4: attachment_id @18");
static_assert(offsetof(SpineDrawItem, skin_id) == 20, "spec 4: skin_id @20");
static_assert(offsetof(SpineDrawItem, kind) == 22, "spec 4: kind @22");
static_assert(offsetof(SpineDrawItem, item_flags) == 23, "spec 4: item_flags @23 (v15)");
static_assert(offsetof(SpineDrawItem, clip_index) == 24, "spec 4: clip_index @24 (v6)");
static_assert(offsetof(SpineDrawItem, vertex_base) == 26, "spec 4: vertex_base @26 (v14)");
static_assert(offsetof(SpineDrawItem, deform_parent) == 28, "spec 4: deform_parent @28 (v14)");
static_assert(offsetof(SpineDrawItem, vertex_count) == 30, "spec 4: vertex_count @30 (v14)");

// ---------------------------------------------------------------------
// 裁剪多边形记录（chunk 9，规格 §7.2）
// ---------------------------------------------------------------------
// 一条 = 一个 ClippingAttachment。**顶点不在这一块里** —— 它复用主顶点缓冲
// （chunk 3）的 rigid / weighted 段，因为裁剪附件是 VertexAttachment，
// 顶点和 mesh 顶点是同一个东西（局部坐标 + 骨骼绑定），连逐字节去重都能共用。
//
// 运行时拿到它之后：用槽位骨骼（rigid）或每个影响自己的骨骼（weighted）
// 把多边形变换到世界空间，再对作用域内的绘制项做 CPU 裁剪。
struct SpineGpuClipPolygon
{
    uint16_t slot_index = 0;                        // @0 —— 裁剪附件所在槽位
    uint16_t skin_id = 0;                           // @2 —— 逐皮肤（换装时作用域也换）
    SpineVertexFormat vertex_format = SpineVertexFormat::kRigid;   // @4
    uint8_t pad = 0;                                // @5
    uint16_t vertex_count = 0;                      // @6
    // @8 —— 进入**所在顶点段**的顶点下标（不是字节偏移）；段由 vertex_format 决定。
    uint32_t vertex_offset = 0;
    // @12（v15）—— 附件名，进字符串表第 9 组（与 chunk 9 一一对应，不去重）。
    //   运行时按"槽位 + 名字"判断槽位**当前**挂的是不是这条裁剪附件。
    uint32_t name_offset = 0;
    // @16（v15）—— 结束槽位（含）；kNoClipEndSlot = 没有（裁到绘制顺序末尾）。
    uint16_t end_slot = SpineGpuConstants::kNoClipEndSlot;
    uint16_t pad2 = 0;                              // @18 —— 落盘必须写 0
};

static_assert(sizeof(SpineGpuClipPolygon) == SpineGpuConstants::kClipPolygonSize,
              "spec 7.2: clip polygon record is 20 B (v15)");
static_assert(offsetof(SpineGpuClipPolygon, slot_index) == 0, "spec 7.2: clip.slot_index @0");
static_assert(offsetof(SpineGpuClipPolygon, skin_id) == 2, "spec 7.2: clip.skin_id @2");
static_assert(offsetof(SpineGpuClipPolygon, vertex_format) == 4, "spec 7.2: clip.vertex_format @4");
static_assert(offsetof(SpineGpuClipPolygon, vertex_count) == 6, "spec 7.2: clip.vertex_count @6");
static_assert(offsetof(SpineGpuClipPolygon, vertex_offset) == 8, "spec 7.2: clip.vertex_offset @8");
static_assert(offsetof(SpineGpuClipPolygon, name_offset) == 12, "spec 7.2: clip.name_offset @12 (v15)");
static_assert(offsetof(SpineGpuClipPolygon, end_slot) == 16, "spec 7.2: clip.end_slot @16 (v15)");
static_assert(offsetof(SpineGpuClipPolygon, pad2) == 18, "spec 7.2: clip.pad2 @18 (v15)");

// ---------------------------------------------------------------------
// 每帧每实例数据（规格 §5）
// ---------------------------------------------------------------------

// 骨骼世界变换：x' = a*x + b*y + worldX，y' = c*x + d*y + worldY。
// 来自 Bone::getA/getB/getC/getD/getWorldX/getWorldY（Bone.h:189-209），
// 即 Skeleton::updateWorldTransform() 之后的值。索引 = skeleton.getBones() 的下标。
struct SpineBoneTransform
{
    float a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f;
    float world_x = 0.0f, world_y = 0.0f;
};

static_assert(sizeof(SpineBoneTransform) == SpineGpuConstants::kBoneStrideCompact,
              "spec 5.1: bone is 24 B (6 float, compact)");
static_assert(offsetof(SpineBoneTransform, a) == 0, "spec 5.1: a @0");
static_assert(offsetof(SpineBoneTransform, b) == 4, "spec 5.1: b @4");
static_assert(offsetof(SpineBoneTransform, c) == 8, "spec 5.1: c @8");
static_assert(offsetof(SpineBoneTransform, d) == 12, "spec 5.1: d @12");
static_assert(offsetof(SpineBoneTransform, world_x) == 16, "spec 5.1: worldX @16");
static_assert(offsetof(SpineBoneTransform, world_y) == 20, "spec 5.1: worldY @20");

// 每可见绘制项一项。CPU 每帧紧凑化：只把当前可见的项追加进来 ⇒
// shader 里不需要任何「是否可见」分支（换装 / 隐藏 / 裁剪 fallback 都由 CPU 决定）。
struct SpineDrawInstance
{
    uint32_t draw_item_index;  // 索引静态绘制项表
    uint32_t bone_base;        // 该实例骨骼表在大 buffer 里的起始骨下标
    uint32_t tint;             // RGBA8，槽位色（直 alpha，非预乘）
    uint32_t dark_tint;        // RGBA8，tintBlack（kUsesTintBlack 才有意义）
    uint32_t root_index;       // @16 —— 索引每帧 root 表（单位是「根变换条目」，不是字节）
};

// 20 B：5 个 u32，无尾部填充（对齐刚好是 4）。
// root_index 追加在**末尾**（而不是紧跟 bone_base）是有意的：保持既有四个字段的
// 偏移不变，序列化代码不用整体挪位。
static_assert(sizeof(SpineDrawInstance) == 20, "spec 5.2: instance item is 20 B (5 x u32)");
static_assert(offsetof(SpineDrawInstance, draw_item_index) == 0, "spec 5.2: draw_item_index @0");
static_assert(offsetof(SpineDrawInstance, bone_base) == 4, "spec 5.2: bone_base @4");
static_assert(offsetof(SpineDrawInstance, tint) == 8, "spec 5.2: tint @8");
static_assert(offsetof(SpineDrawInstance, dark_tint) == 12, "spec 5.2: dark_tint @12");
static_assert(offsetof(SpineDrawInstance, root_index) == 16, "spec 5.2: root_index @16");

// 根变换（规格 §5.3）：实例自身的世界变换（UI 里的位置 / 缩放 / 旋转），
// 每实例一个，存在**每帧 root 表**里，由 SpineDrawInstance::root_index 索引。
//
// 施加顺序是**蒙皮之后**：p = root.apply(skinned)。
// 不能预乘进骨骼表 —— 骨骼表存的是骨骼世界变换，把 root 乘进去会二次缩放。
//
// 与 SpineBoneTransform 完全同构（都是 2×3 仿射、都是 24 B），但**语义不同**：
// 一个是骨骼世界变换、一个是实例根变换。保持两个类型是为了防止互相换用 ——
// 换用了能编译、能跑，只是角色整体位移/缩放悄悄错。
struct SpineRootTransform
{
    float a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f;  // x' = a*x + b*y + tx
    float tx = 0.0f, ty = 0.0f;                    // y' = c*x + d*y + ty
};

static_assert(sizeof(SpineRootTransform) == SpineGpuConstants::kRootStrideCompact,
              "spec 5.3: root is 24 B (6 float, compact)");
static_assert(offsetof(SpineRootTransform, a) == 0, "spec 5.3: a @0");
static_assert(offsetof(SpineRootTransform, b) == 4, "spec 5.3: b @4");
static_assert(offsetof(SpineRootTransform, c) == 8, "spec 5.3: c @8");
static_assert(offsetof(SpineRootTransform, d) == 12, "spec 5.3: d @12");
static_assert(offsetof(SpineRootTransform, tx) == 16, "spec 5.3: tx @16");
static_assert(offsetof(SpineRootTransform, ty) == 20, "spec 5.3: ty @20");
static_assert(std::is_standard_layout_v<SpineRootTransform> &&
                  std::is_standard_layout_v<SpineBoneTransform>,
              "root and bone must stay standard-layout (they are memcpy'd / read as raw bytes)");
