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
    constexpr uint32_t kFormatVersion = 8;

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

    // 静态绘制项大小（规格 §4）。v3 起是 24 B（多了 uv_offset），v6 起是 28 B（多了 clip_index）。
    // 与 kDrawInstanceSize 别互相换用 —— 一个是静态表项、一个是每帧实例项。
    constexpr uint32_t kDrawItemSize = 28;

    // 裁剪多边形记录大小（chunk 9，规格 §7.2）。
    constexpr uint32_t kClipPolygonSize = 12;

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
    kChunkClipData = 9,      // SpineGpuClipPolygon[]（12 B/条）；**顶点本身在 chunk 3 里**

    // v8：骨骼 setup（36 B/骨）。id 10 就是当初留给它的。
    kChunkBoneSetup = 10,    // SpineGpuBone[]（36 B/条）；下标 = header.bone_count 的序

    // v7：蒙皮数据流。id 从 21 起 —— 10..20 留给 v7 动画阶段
    // （骨骼树 / 时间轴 / path 几何），别占用。
    kChunkInfluenceBind = 21,  // float2 × influence_count（8 B/条）；由 influence_offset 索引
    kChunkInfluenceBone = 22,  // SpineInfluenceBone[]（4 B/条）；与 21 **共用同一个条目下标**
};

// ---------------------------------------------------------------------
// 容器头（64 B，规格 §9）
// ---------------------------------------------------------------------
struct SpineGpuHeader
{
    uint32_t magic;             // 'ZSPG'
    uint32_t version;           // 本规格版本，当前 8（见 kFormatVersion 的版本史）
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
// 绘制项表（静态，规格 §4）
// ---------------------------------------------------------------------
// 合批键 = (atlas_page, blend_mode, vertex_format, kind)。
struct SpineDrawItem
{
    uint32_t index_offset;
    uint32_t index_count;
    // ⚠️ **UV 条目的下标，不是字节偏移**（chunk 4 里一条 8 B）。
    // 顶点可以被多个绘制项共用（几何相同），但 UV 各取各的 ⇒ 必须有这一项。
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
    uint8_t pad;                       // @23
    // @24 —— 这一项被哪条裁剪多边形裁（chunk 9 的行号）。**不被裁时是 kNoClipIndex**。
    // 为什么要存下标而不是让运行时按 (skin_id, slot_index) 去查：一个槽位在**静态上**
    // 可能被多条裁剪覆盖过（不同皮肤 / 嵌套），绘制项要的是"当时生效的那一条"，
    // 而这个只有导入期模拟状态机才知道。
    uint16_t clip_index;
    uint16_t pad2;                     // @26 —— 凑 28 B，落盘必须写 0
};

// v3 起是 **24 B**（20 B + uv_offset）。
// 序列化时两端都必须按 **24 B** 落盘并显式把未用字节写 0，否则 C# 与 C++ 会对不上。
static_assert(sizeof(SpineDrawItem) == SpineGpuConstants::kDrawItemSize,
              "spec 4: draw item serializes as 24 B");
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
static_assert(offsetof(SpineDrawItem, clip_index) == 24, "spec 4: clip_index @24 (v6)");
static_assert(offsetof(SpineDrawItem, pad2) == 26, "spec 4: pad2 @26 (v6)");

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
};

static_assert(sizeof(SpineGpuClipPolygon) == SpineGpuConstants::kClipPolygonSize,
              "spec 7.2: clip polygon record is 12 B");
static_assert(offsetof(SpineGpuClipPolygon, slot_index) == 0, "spec 7.2: clip.slot_index @0");
static_assert(offsetof(SpineGpuClipPolygon, skin_id) == 2, "spec 7.2: clip.skin_id @2");
static_assert(offsetof(SpineGpuClipPolygon, vertex_format) == 4, "spec 7.2: clip.vertex_format @4");
static_assert(offsetof(SpineGpuClipPolygon, vertex_count) == 6, "spec 7.2: clip.vertex_count @6");
static_assert(offsetof(SpineGpuClipPolygon, vertex_offset) == 8, "spec 7.2: clip.vertex_offset @8");

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
