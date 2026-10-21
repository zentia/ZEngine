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
    constexpr uint32_t kFormatVersion = 5;

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

    // 每顶点影响数上限。导入器遇到 n > 4 必须**报错并报出附件名**，
    // 不允许静默截断 —— 静默截断 = 形状悄悄错（规格 §3.1）。
    constexpr uint32_t kMaxInfluences = 4;

    // 骨骼表步长：6 float 紧凑（默认）或 32 B（std140 UBO 兼容变体）。
    constexpr uint32_t kBoneStrideCompact = 24;
    constexpr uint32_t kBoneStrideStd140 = 32;

    // 根变换步长。root 与骨骼同构（都是 2×3 仿射），std140 变体跟着 bone_stride 走。
    constexpr uint32_t kRootStrideCompact = 24;
    constexpr uint32_t kRootStrideStd140 = 32;

    // 每帧实例项大小（含 root_index，规格 §5.2）。
    constexpr uint32_t kDrawInstanceSize = 20;

    // 静态绘制项大小（规格 §4）。v3 起是 24 B（多了 uv_offset）。
    // 与 kDrawInstanceSize 别互相换用 —— 一个是静态表项、一个是每帧实例项。
    constexpr uint32_t kDrawItemSize = 24;

    // 顶点步长。**UV 已经不在这里了**（v3 起它在独立的 chunk 4 里）⇒
    //   rigid    : bindPos 2 float                = 8 B
    //   weighted : bindPos 4×2 + boneIdx 4×u16 + weight 4×u8 + pad = 48 B
    // weighted 是 48 不是 44：ByteAddressBuffer::Load2 要求 8 字节对齐，
    // 步长 44 时第 k 个顶点落在 44k，44 % 8 == 4 ⇒ 第二个及以后的顶点取不到对齐的
    // Load2，只能退化成标量 Load。4 B/顶点（约 9%）换对齐的向量化读取（规格 §3.1）。
    constexpr uint32_t kRigidVertexStride = 8;
    constexpr uint32_t kWeightedVertexStride = 48;

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
}   // namespace SpineGpuConstants

// ---------------------------------------------------------------------
// 枚举
// ---------------------------------------------------------------------

// 静态 VB 段内的顶点格式。两种并存，不要统一成一种（规格 §3.1）。
enum class SpineVertexFormat : uint8_t
{
    kRigid = 0,      // _bones 为空，整块由槽位骨骼单矩阵变换
    kWeighted4 = 1,  // 每顶点最多 4 个影响，每个影响有自己的绑定坐标
};

// 与 spine-cpp BlendMode.h:35-38 逐值对应（Normal=0 / Additive / Multiply / Screen）。
enum class SpineBlendMode : uint8_t
{
    kNormal = 0,
    kAdditive = 1,
    kMultiply = 2,
    kScreen = 3,
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
    kChunkClipData = 9,      // 裁剪多边形（CPU fallback 用）
};

// ---------------------------------------------------------------------
// 容器头（64 B，规格 §9）
// ---------------------------------------------------------------------
struct SpineGpuHeader
{
    uint32_t magic;             // 'ZSPG'
    uint32_t version;           // 本规格版本，当前 5（见 kFormatVersion 的版本史）
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
struct SpineWeightedVertex
{
    float bind_pos[SpineGpuConstants::kMaxInfluences][2];  // @0，每影响一份
    uint16_t bone_idx[SpineGpuConstants::kMaxInfluences];  // @32
    uint8_t weight[SpineGpuConstants::kMaxInfluences];     // @40，unorm8
    uint8_t pad[4];                                        // @44
};

static_assert(sizeof(SpineWeightedVertex) == SpineGpuConstants::kWeightedVertexStride,
              "spec 3.1: kVtxWeighted4 is 48 B (uv moved out)");
static_assert(offsetof(SpineWeightedVertex, bind_pos) == 0, "spec 3.1: bindPos @0");
static_assert(offsetof(SpineWeightedVertex, bone_idx) == 32, "spec 3.1: boneIdx @32");
static_assert(offsetof(SpineWeightedVertex, weight) == 40, "spec 3.1: weight @40");
static_assert(offsetof(SpineWeightedVertex, pad) == 44, "spec 3.1: pad @44");
static_assert(alignof(SpineWeightedVertex) == 4, "weighted vertex must stay 4-byte aligned");
static_assert(SpineGpuConstants::kWeightedVertexStride % 8 == 0,
              "weighted stride must be 8-byte aligned (Load2 on bindPos, spec 3.1)");
// 把「3.8 ⇒ 骨骼索引是 uint16」钉成编译期断言。
static_assert(std::is_same_v<decltype(SpineWeightedVertex::bone_idx),
                             uint16_t[SpineGpuConstants::kMaxInfluences]>,
              "Spine 3.8: _bones is Vector<unsigned short> -> bone index must be uint16 (spec 1.1)");

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
