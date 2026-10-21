#pragma once

// =====================================================================
// GPU-Spine 导入器：Spine 3.8（json + atlas）→ 中间容器
// 规格：doc/GPU_SPINE_INTERCHANGE_FORMAT.md
// ---------------------------------------------------------------------
// 刻意分成两级，因为两级的失败模式完全不同：
//
//   Parse*()  —— 忠实翻译 spine 的数据。出错是"读不出来"，明显、立刻可见。
//   Bake*()   —— 切成 GPU 布局。出错是"能编译、能跑、能画，形状悄悄错"，
//                几乎不可能在运行时定位。
//
// 所以 Bake 里每一条"静默就会错"的规则都配**显式报错**：
//   * 影响数 > 4            → 报错并报出附件名，绝不静默截断
//   * 权重和调不到 255      → 报错
//   * 骨骼索引 > 65535      → 报错（3.8 的 _bones 是 unsigned short，
//                             截断会直接毁掉蒙皮，参考实现里也是 assert）
//   * 缺图集 region         → 报错，不拿零 UV 糊过去
// =====================================================================

#include "Runtime/Function/Render/Spine/SpineGpuFormat.h"

#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------
// 图集：UV 的唯一来源
// ---------------------------------------------------------------------
// Spine 的 mesh `uvs` 是**图集空间**的 regionUV，必须按图集页归一化成 0..1
// （参考实现 `MeshAttachment::updateUVs()`），region 附件的 UV 则由
// `RegionAttachment::setUVs()` 的规则决定 —— 两者都躲不开图集。
struct SpineAtlasPage
{
    std::string name;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct SpineAtlasRegion
{
    std::string name;
    uint32_t page = 0;
    float u = 0.0f, v = 0.0f, u2 = 0.0f, v2 = 0.0f;
    bool rotate = false;

    // 像素尺寸 —— 烘焙 UV 时躲不开：
    //   width/height = 打包后的尺寸（可能被裁剪掉透明边）
    //   orig_w/orig_h = 裁剪前的原始尺寸
    //   offset_x/offset_y = 原始图左上角相对打包区域的偏移
    // 不要为了省事假设"未裁剪"：一旦图集带裁剪，UV 会整体偏移且**不报错**。
    int width = 0, height = 0;
    int orig_w = 0, orig_h = 0;
    int offset_x = 0, offset_y = 0;
};

struct SpineAtlas
{
    std::vector<SpineAtlasPage> pages;
    std::vector<SpineAtlasRegion> regions;
};

struct SpineImportResult
{
    bool ok = false;
    std::string error;   // ok == false 时的原因，带附件名 / 槽位名
};

SpineImportResult ParseSpineAtlas(const char* text, size_t len, SpineAtlas& out);
const SpineAtlasRegion* FindAtlasRegion(const SpineAtlas& atlas, const std::string& name);

// ---------------------------------------------------------------------
// 源骨架：解析完的中间形态（还是 spine 自己的数据形状）
// ---------------------------------------------------------------------
struct SpineSourceBone
{
    std::string name;
    int parent = -1;
};

struct SpineSourceSlot
{
    std::string name;
    int bone_index = -1;
    bool has_dark = false;
};

struct SpineSourceAttachment
{
    enum class Type : uint8_t
    {
        kRegion = 0,
        kMesh = 1,
        kClipping = 2,
    };

    Type type = Type::kRegion;
    std::string name;            // 附件名（skin 里的 key）
    std::string path;            // 图集 region 名；空则退回 name
    int slot_index = -1;
    // 所属皮肤在 `SpineSourceSkeleton::skins` 里的下标。**不再合并皮肤**：
    // 同一个 (槽位, 附件名) 在不同皮肤下是两份几何 —— 合并会丢掉换装。
    int skin_index = -1;

    // spine 原样的扁平顶点数组，两种布局共用（参考实现 SkeletonJson::readVertices）：
    //   rigid    → [x, y, x, y, ...]                （vertices.size() == uvs.size()）
    //   weighted → 每顶点 [n, (boneIdx, x, y, w) × n]
    // 判据就是 `vertices.size() != uvs.size()`，不是靠某个 type 字段。
    std::vector<float> vertices;

    // 每顶点 2 个，已按图集页算成最终 UV。顶点顺序与 triangles 的索引一致。
    std::vector<float> uvs;
    std::vector<uint16_t> triangles;

    // mesh 的**原始** regionUV（图集空间，未归一化），只有 linked mesh 非它不可：
    // linked mesh 继承父附件的 regionUVs，却要用**自己的**图集 region 重算最终 UV
    // （`MeshAttachment::setParentMesh()` 拷 _regionUVs，`updateUVs()` 用自己的
    //  _regionU/V/…，见 MeshAttachment.cpp:226-239 与 :64-92）。
    // region 附件没有这个（它的 UV 来自 `setUVs()`，与 mesh 的公式不同）。
    std::vector<float> region_uvs;

    bool IsWeighted() const { return vertices.size() != uvs.size(); }
    uint32_t VertexCount() const { return static_cast<uint32_t>(uvs.size()) / 2u; }
};

struct SpineSourceSkeleton
{
    std::vector<SpineSourceBone> bones;
    std::vector<SpineSourceSlot> slots;
    // 按 (皮肤, 绘制顺序) 排好 —— 先皮肤 0（default）的全部附件，再皮肤 1……
    // 皮肤内的顺序 = 槽位顺序（同一槽位同一时刻只有一个附件可见）。
    std::vector<SpineSourceAttachment> attachments;

    // 皮肤名，下标 = 容器里的 skin_id。**id 0 必须是 default 皮肤**
    // （SpineGpuFormat.h 里 kDefaultSkinId 的注释讲了为什么必须显式检查）。
    std::vector<std::string> skins;

    // flags 来源
    bool has_draw_order_timeline = false;   // animations.*.drawOrder 存在 ⇒ 整个资产走 CPU
    bool uses_tint_black = false;           // 任一槽位带 dark
    bool has_clipping = false;              // 存在 clipping 附件

    // 图集页名（写进容器 chunk 7；序列化时才知道纹理引用）
    std::vector<std::string> atlas_pages;
};

// 解析 Spine 3.8 的 json 导出。atlas 可为 nullptr —— 但那样所有需要 UV 的附件
// 都会**报错**（不是给零 UV 糊过去）。
SpineImportResult ParseSpineSkeletonJson(const char* json, size_t len, const SpineAtlas* atlas,
                                         SpineSourceSkeleton& out);

// 解析 Spine 3.8 的**二进制** `.skel` 导出（Spine 编辑器的默认导出格式）。
// 产出与 json 版**同一个** SpineSourceSkeleton —— 后面的 Bake 完全不知道数据来自哪种。
SpineImportResult ParseSpineSkeletonBinary(const uint8_t* data, size_t len, const SpineAtlas* atlas,
                                           SpineSourceSkeleton& out);

// ---------------------------------------------------------------------
// 烘焙产物（容器在内存里的形态；序列化到文件是下一步）
// ---------------------------------------------------------------------
struct SpineGpuAsset
{
    SpineGpuHeader header {};

    // 顶点缓冲按格式分段：先所有 rigid 段、再所有 weighted 段。
    // 每段起点 8 B 对齐（为了对齐的 ByteAddressBuffer::Load2，规格 §3.1）。
    // **不含 UV** —— 顶点只存几何 + 蒙皮，几何相同的附件**共用**同一段顶点。
    std::vector<uint8_t> vertex_buffer;
    uint32_t rigid_vertex_count = 0;
    uint32_t weighted_vertex_count = 0;
    uint32_t rigid_segment_offset = 0;
    uint32_t weighted_segment_offset = 0;

    // UV 通道（chunk 4）：每条目 2 float。**条目数 ≠ 顶点数** ——
    // 顶点被共用时，每个绘制项各有一份 UV（这正是拆通道换来的省法）。
    // 由 SpineDrawItem::uv_offset 索引。
    std::vector<float> uv_buffer;

    std::vector<uint16_t> index_buffer;
    std::vector<SpineDrawItem> draw_items;
    std::vector<uint16_t> slot_bone;          // 每槽位的骨骼下标（rigid 用）

    // 字符串表（chunk 1）。落盘顺序固定：骨骼名 → 皮肤名 → 槽位名 → 附件名 → 图集页名，
    // 每项以 '\0' 结尾。读回时按 bone_count / skin_count / slot_count / 附件记录数切分。
    std::vector<std::string> bone_names;
    std::vector<std::string> skin_names;
    std::vector<std::string> slot_names;
    std::vector<std::string> attachment_names;
    std::vector<std::string> atlas_page_names;

    // 附件记录（chunk 6）。Bake 时建好；序列化 / 反序列化都走它 ——
    // 不再从 draw_items 反推（那会假设"附件与绘制项一一对应"，是个脆假设）。
    std::vector<SpineGpuAttachmentRecord> attachment_records;
};

SpineImportResult BakeSpineGpuAsset(const SpineSourceSkeleton& src, SpineGpuAsset& out);

// ---------------------------------------------------------------------
// 容器序列化（规格 §9）
// ---------------------------------------------------------------------
// 附件记录 SpineGpuAttachmentRecord 定义在 SpineGpuFormat.h —— 它是容器布局的
// 一部分，C# 端要照抄，放在"布局的唯一权威定义"里才不会两边漂移。

SpineImportResult WriteSpineGpuContainer(const SpineGpuAsset& asset, std::vector<uint8_t>& out);
SpineImportResult ReadSpineGpuContainer(const uint8_t* data, size_t len, SpineGpuAsset& out);
