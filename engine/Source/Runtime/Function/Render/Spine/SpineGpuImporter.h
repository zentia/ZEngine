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

// 源数据的 spine 版本。**只认 3.7 与 3.8**：两者的 .skel 字节布局经逐段比对完全一致
// （骨骼 / 槽位 / IK / transform / path / 皮肤 / 附件 / 事件 / 动画），
// 唯一的差别是槽位的暗色判据 —— 所以是"同一个解析器 + 一个分支"，不是两套。
//
//   * 3.7（`spine-runtimes` 3.7 `spine-csharp/src/SkeletonBinary.cs:190-192`）：
//         int darkColor = ReadInt(input);
//         if (darkColor != -1) slotData.hasSecondColor = true;
//   * 3.8（`spine-cpp/src/spine/SkeletonBinary.cpp:186-189`）：
//         读 4 个字节，!(r==0xff && g==0xff && b==0xff && a==0xff) 才算有暗色。
//
// ⚠️ 3.7 的 `-1` 就是 0xFFFFFFFF，但 3.8 的判据是"四个字节不全为 ff" ——
//    3.8 里 `0xFFFFFF00`（a=0）会被判为**有**暗色，3.7 的 `!= -1` 同样判为有；
//    差别只出现在 `0xFFFFFFFF`：3.7 判无、3.8 也判无。真正的分歧是**语义**而不只是编码，
//    所以必须按版本分别走，不能取巧合并（3.7 没有 alpha 分量，写死 1）。
//
// 4.x 不在此列：`_bones` 从 `Vector<unsigned short>` 变成 `Vector<int>`，
// 顶点布局不同，需要另一条导入路径（规格 §1.1）。
enum class SpineSourceVersion : uint8_t
{
    kUnknown = 0,
    k37 = 37,
    k38 = 38,
};

// 骨骼 setup。字段与 `BoneData.h:110-115` 一一对应，默认值抄
// `SkeletonJson.cpp:165-172`（二进制没有默认值 —— 8 个 float 全是显式写入的）。
//
// ⚠️ scale 的默认是 **1** 不是 0。写 0 的话整棵子树会退化成一个点，
//    而"看起来只是这部分没画出来"是不报错的。
//
// ⚠️ `transform_mode` 的 json 字段名是 **"transform"**（不是 "transformMode"），
//    值是那 5 个驼峰串；认不出的串参考实现会**静默留在 Normal**
//    （SkeletonJson.cpp:173-189，5 个 if 都没有 else）⇒ 我们报显式错。
struct SpineSourceBone
{
    std::string name;
    int parent = -1;

    float x = 0.0f;
    float y = 0.0f;
    float rotation = 0.0f;    // 度
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    float shear_x = 0.0f;
    float shear_y = 0.0f;
    float length = 0.0f;      // IK / 编辑器挂点；不参与蒙皮
    SpineTransformMode transform_mode = SpineTransformMode::kNormal;
};

// 槽位 setup。字段与 `SlotData.cpp:39-41` 的默认构造一一对应：
//   `_color(1,1,1,1)`、`_darkColor(0,0,0,0)`、`_hasDarkColor(false)`。
//
// ⚠️ `dark_a` **恒为 1**（只要这个槽位有暗色）—— 参考实现写死的：
//   * 二进制 `SkeletonBinary.cpp:188`：`getDarkColor().set(r/255, g/255, b/255, 1)`
//   * json    `SkeletonJson.cpp:230` ：`darkColor.a = 1`
//   二进制里读进来的第 4 个字节**只用来判"有没有暗色"**（3.8 判据：四分量不全为 0xff），
//   从不作为 alpha 落地。照抄这个不对称，别把它当"读漏了一个字节"去补。
//
// ⚠️ json 的 dark 只有 **rgb 三个分量**（`SkeletonJson.cpp:227-230` 只读 0/1/2），
//    与动画帧里 twoColor 的 dark 一样（`SkeletonJson.cpp:863-866`）。
struct SpineSourceSlot
{
    std::string name;
    int bone_index = -1;
    bool has_dark = false;
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float dark[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    // setup 附件名（`SlotData::_attachmentName`）。空串 = 这个槽位默认不挂附件。
    // ⚠️ 它是 attachment 时间轴的**基线**：`AttachmentTimeline::apply` 在
    //    `time < frames[0]` 或 direction==Out 时把槽位复位到它 —— 没有它，
    //    "动画开始前该挂什么"就无从回答（默认成"什么都不挂"会静默少画东西）。
    std::string attachment_name;
    // 混合模式（`SlotData::_blendMode`，BlendMode.h:35-38）。烘进该槽位每个绘制项的
    // `SpineDrawItem::blend_mode`。以前两条解析路径都把它丢了（二进制读了不存、json 不读），
    // 绘制项一律 kNormal ⇒ 加色特效全画成普通混合，且不报错。
    SpineBlendMode blend_mode = SpineBlendMode::kNormal;
};

// **没有绘制项**的附件：boundingbox / path / point / clipping。
//
// 记下来的唯一用途是 deform 时间轴**可以指向它们** —— 四者都是 `VertexAttachment`
// （`ClippingAttachment.h` / `PathAttachment.h` 都继承它），而参考实现的
// `DeformTimeline::apply` 只检查 `instanceOf(VertexAttachment::rtti)`，不检查类型。
//
// 本工程实测（782 个资产）：
//   * 88 处 deform 指向 **path**（`K_Lvbu` 槽位 121、`K_Direnjie` 槽位 root..root5）
//   * 4 处指向 **clipping**（`K_Niumo` 槽位 shandian1）
//
// ⇒ "deform 指向一个没有绘制项的附件" **不是错误**：
//   * path / boundingbox / point：根本不渲染，deform 无从生效；
//   * clipping：它本来就走 CPU fallback（§7.2），多边形每帧由那条路径重算，
//     deform 由它处理 —— 容器里存的是未变形的多边形，不需要额外标记。
//   把它们当错误会让 50+ 个资产整个导不出来，而理由是"某个看不见的东西被动画了"。
//
// 但"deform 指向一个**会产生绘制项**却找不到的附件"仍然是错误 —— 那才是真丢了数据。
//
// ⚠️ v7 的 S4 做 path 约束时需要 path 的几何（chunk 20），那时 path 要从
//    "不烘焙"升级成"烘焙"，但**仍然不产生绘制项**。
struct SpineSourceNoDrawAttachment
{
    int skin_index = -1;
    int slot_index = -1;
    std::string name;
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
    // 这个附件的贴图在图集第几页（= `SpineAtlasRegion::page`）。烘进 `SpineDrawItem::atlas_page`，
    // 渲染侧按它选纹理。以前绘制项一律写 0 —— 多页图集会全部采第 0 页，且不报错。
    uint32_t atlas_page = 0;
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

    // linked mesh 的父附件在 SpineSourceSkeleton::attachments 里的下标；
    // **非 linked mesh 恒为 -1**。deform 要沿它传播（见下面 inherit_deform）。
    // ⚠️ 只在解析期内有效：随后 attachments 会重排，别拿它当持久引用。
    int parent_index = -1;

    // linked mesh 是否继承父附件的 deform。json 字段名是 **"deform"**（不是
    // "inheritDeform"），默认 1（SkeletonJson.cpp:572）；二进制是一个 bool 字段
    // （SkeletonBinary.cpp:594）。判据是 MeshAttachment::applyDeform()（:94-95）：
    //   this == source || (inheritDeform && parentMesh == source)
    bool inherit_deform = true;

    // 有 deform（FFD）时间轴命中它 ⇒ 顶点每帧都变 ⇒ 画不动静态 VB ⇒ 走 CPU fallback
    // （规格 §7.1）。注意：**不是整个资产**回退，只有被命中的绘制项。
    bool deformed = false;

    bool IsWeighted() const { return vertices.size() != uvs.size(); }
    uint32_t VertexCount() const { return static_cast<uint32_t>(uvs.size()) / 2u; }
};

// 一个裁剪附件（ClippingAttachment）的**作用域**（规格 §7.2）。
//
// 作用域是 **(slot_index, end_slot_index]** —— 半开半闭，两头都不对称：
//   * 裁剪附件自己那个槽位**不**被裁：参考实现一命中 clipping 就 `clipStart()` 然后
//     `continue`，该槽位根本不画（MeshGeneration/SpineMesh.h:153-159）。
//   * 结束槽位**含在内**：槽位收尾调 `clipEnd(slot)`，而它只在
//     `_clipAttachment->_endSlot == &slot._data` 时才真的收尾（SkeletonClipping.cpp:72-76）。
struct SpineSourceClip
{
    int skin_index = -1;
    int slot_index = -1;
    // 结束槽位（含）。**< 0 表示"没有结束槽位"** —— json 没写 "end" 或名字查不到时
    // `_endSlot` 是 NULL，`clipEnd(slot)` 永不成立 ⇒ 一路裁到最后一个槽位
    // （只剩整条绘制指令末尾那次无条件 `clipEnd()`，SpineMesh.h:259）。
    int end_slot_index = -1;

    // 裁剪多边形本身（局部坐标 + 骨骼绑定）。它是 **VertexAttachment**，顶点与 mesh
    // 顶点同构 —— 所以在 Bake 里复用同一套编码与去重，顶点进主顶点缓冲（规格 §7.2）。
    //   rigid    → [x, y, x, y, ...]，顶点数 = vertices.size() / 2
    //   weighted → 每顶点 [n, (boneIdx, x, y, w) × n]，顶点数 = vertex_count
    std::vector<float> vertices;
    int vertex_count = 0;
    bool weighted = false;
};

// ---------------------------------------------------------------------
// 约束（源形态）—— S4
// ---------------------------------------------------------------------
// 字段集合逐条抄 3.7：
//   * IK       `SkeletonBinary.cpp:190-208` / `SkeletonJson.cpp:263-294`
//   * transform `SkeletonBinary.cpp:217-237` / `SkeletonJson.cpp:307-345`
//   * path      `SkeletonBinary.cpp:239-257` / `SkeletonJson.cpp:360-422`
//
// ⚠️ 三条的 `target` **不是同一个下标空间**：IK / transform 指向**骨骼**，
//    path 指向**槽位**（`SkeletonBinary.cpp:249` 取的是 `_slots[...]`）。
//
// ⚠️ `order` 是**三类约束共用的排名空间**：`Skeleton::updateCache()`
//    （`Skeleton.cpp:147-175`）按 i = 0..N-1 逐个找 `order == i` 的那条约束
//    （先扫 ik、再 transform、再 path），找不到就**跳过**。
//    ⇒ order 不是 0..N-1 的一个排列时，参考实现会**静默丢掉**约束；
//      我们报显式错（见 Bake）。
//
// ⚠️ `position` / `spacing` 的缩放是**解析期**的事：
//    `positionMode == Fixed` / `spacingMode ∈ {Length, Fixed}` 时才乘 `_scale`
//    （`SkeletonBinary.cpp:252-255`、`SkeletonJson.cpp:410-417`）。
//    落盘的是**已经乘过**的值，运行时不要再乘一次。
struct SpineSourceIkConstraint
{
    std::string name;
    int order = 0;
    std::vector<int> bones;      // 骨骼下标，**至少 1 条**
    int target = -1;             // 目标骨骼
    float mix = 1.0f;
    int bend_dir = 1;            // 1 或 -1（json 是 `bendPositive` 布尔）
    bool compress = false;
    bool stretch = false;
    bool uniform = false;
};

struct SpineSourceTransformConstraint
{
    std::string name;
    int order = 0;
    std::vector<int> bones;
    int target = -1;             // 目标骨骼
    bool local = false;
    bool relative = false;
    float offset_rotation = 0.0f;
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    float offset_scale_x = 0.0f;
    float offset_scale_y = 0.0f;
    float offset_shear_y = 0.0f;
    float rotate_mix = 1.0f;
    float translate_mix = 1.0f;
    float scale_mix = 1.0f;
    float shear_mix = 1.0f;
};

struct SpineSourcePathConstraint
{
    std::string name;
    int order = 0;
    std::vector<int> bones;
    // ⚠️ **槽位**下标不是骨骼（`SkeletonBinary.cpp:249`）
    int target_slot = -1;
    int position_mode = 1;   // 0 fixed / 1 percent（json 默认 "percent"）
    int spacing_mode = 0;    // 0 length / 1 fixed / 2 percent（json 默认 "length"）
    int rotate_mode = 0;     // 0 tangent / 1 chain / 2 chainScale（json 默认 "tangent"）
    float offset_rotation = 0.0f;
    float position = 0.0f;
    float spacing = 0.0f;
    float rotate_mix = 1.0f;
    float translate_mix = 1.0f;
};

// PathAttachment 的几何。它**不产生绘制项**，但 path 约束要沿它摆骨骼
// （`PathConstraint::computeWorldPositions` 走 `PathAttachment::computeWorldVertices`）。
//
// 顶点编码与 mesh **同构**（`SkeletonBinary::readVertices`，:1108-1130）：
//   rigid    → vertices = [x, y] × vertex_count，bones 空
//   weighted → bones = [n, idx × n] × vertex_count，vertices = [x, y, w] × Σn
// lengths 长度 = vertex_count / 3（`SkeletonBinary.cpp:550`）。
struct SpineSourcePathAttachment
{
    int skin_index = -1;
    int slot_index = -1;
    std::string name;
    bool closed = false;
    bool constant_speed = true;   // json 默认 **1**（SkeletonJson.cpp:596）
    int vertex_count = 0;
    bool weighted = false;
    std::vector<float> vertices;   // rigid [x,y]×n / weighted [x,y,w]×Σn
    std::vector<int> bones;        // weighted 才有：[n, idx×n]×n
    std::vector<float> lengths;
};

// ---------------------------------------------------------------------
// 动画（源形态，仍是 spine 自己的数据形状）
// ---------------------------------------------------------------------

// 一段曲线（帧 i → 帧 i+1）。type 与控制点逐值抄 `CurveTimeline.cpp:42-44`/`:77-95`。
//
// ⚠️ 二进制 `readCurve()`（SkeletonBinary.cpp:1112-1127）的 switch **没有 default**：
//    认不出的字节会**静默当成 linear**。我们报显式错 —— 认不出的曲线字节意味着
//    "这段插值方式和 spine 里不一样"，静默当 linear 会让动作悄悄变样。
struct SpineSourceCurve
{
    SpineCurveType type = SpineCurveType::kLinear;
    // 只有 kBezier 有意义。默认全 0（= 一条合法的、退化的 bezier，不会残留脏字节）。
    float cx1 = 0.0f;
    float cy1 = 0.0f;
    float cx2 = 0.0f;
    float cy2 = 0.0f;
};

// deform 的一帧。**必须保持稀疏**：只记 `[start, start + values.size())` 这一段。
//
// `values` 为空 = **回退帧**（参考实现里 `end == 0` 那一路，SkeletonBinary.cpp:990）。
// 回退的含义对 rigid / weighted **不一样**，但不影响这里的存储：
//   * rigid    → 参考实现填的是**绝对**局部坐标，整体替换（VertexAttachment.cpp:72-73）
//   * weighted → 填 0，逐影响**相加**（:122-123）
// 两者的"不动"都等于顶点缓冲里那份绑定姿势 ⇒ 读端遇到空帧就是"顶点不改"。
struct SpineSourceDeformFrame
{
    uint32_t start = 0;
    std::vector<float> values;
};

// drawOrder 的一帧：只存**被挪动**的槽位（抄 SkeletonBinary.cpp:1051-1072 的
// offsetCount + (slotIndex, offset) 流；没列出的槽位按 setup 顺序填空）。
struct SpineSourceDrawOrderFrame
{
    std::vector<uint32_t> slots;
    // 与 slots 一一对应。**有符号**：参考实现是 `drawOrder[index + readVarint()] = ...`，
    // 而 varint 是有符号读法（zigzag）⇒ 偏移可以是负的。
    std::vector<int32_t> offsets;
};

struct SpineSourceTimeline
{
    SpineTimelineType type = SpineTimelineType::kRotate;
    // 骨骼 / 槽位 / 约束下标（deform 在 Bake 期才由 (皮肤, 槽位, 附件名) 换算成
    // 附件记录下标，所以这里先留 -1）。
    int target = 0;

    // 定长部分：frame_count × SpineTimelineEntries(type) 个 float，行主序。
    // 变长的三类（attachment / deform / drawOrder / event）这里只有 time（1 个/帧）。
    std::vector<float> frames;

    // 段数 = frame_count - 1；attachment / drawOrder / event **没有**曲线（不是
    // CurveTimeline），这里是空的。
    std::vector<SpineSourceCurve> curves;

    // ---- 变长部分（与 frames 一一对应）----
    // type 4 attachment：每帧一个附件名；**空串 = 该帧隐藏这个槽位**。
    std::vector<std::string> attachment_names;
    // type 6 deform：每帧一段稀疏值。
    std::vector<SpineSourceDeformFrame> deform_frames;
    // type 6 的寻址键（deform 是 (皮肤, 槽位, 附件) 三级）。
    int deform_skin = -1;
    int deform_slot = -1;
    std::string deform_attachment;
    // type 8 drawOrder：每帧一组偏移。
    std::vector<SpineSourceDrawOrderFrame> draw_order_frames;
};

struct SpineSourceAnimation
{
    std::string name;
    std::vector<SpineSourceTimeline> timelines;
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

    // 解析到了但不烘焙的附件（boundingbox / path / point）。见上面的注释：
    // 存在的意义是让"deform 指向它们"被判成"无事发生"而不是"错误"。
    std::vector<SpineSourceNoDrawAttachment> no_draw_attachments;

    // 裁剪作用域（规格 §7.2）。Bake 按它把**作用域内**的绘制项标成 kFallback ——
    // 不是整个资产回退。作用域是逐皮肤的：裁剪附件本身是某个皮肤下的槽位附件，
    // 换到没有它的皮肤时作用域也就不存在（3.8 同时只有一个皮肤生效）。
    std::vector<SpineSourceClip> clips;

    // 动画（v9：烘焙进 chunk 11-16）。json 与二进制走同一个 SpineSourceTimeline。
    std::vector<SpineSourceAnimation> animations;

    // 约束**名**的有序表。只用于把 json 里 `animations.*.ik / transform / path` 的
    // **名字**换算成下标（参考实现 `findIkConstraint()` 找不到时把下标**静默留在 0**，
    // SkeletonJson.cpp:933-940 ⇒ 我们报显式错）。
    //
    // ⚠️ 顺序必须与 iks / transforms / paths **一致**：时间轴里的约束下标、
    // chunk 18 里各类数组的下标、chunk 19 里 `SpineGpuUpdateStep::index`
    // 用的是同一个下标空间。
    // ⚠️ 二进制**不需要**它做换算：二进制的时间轴里写的就是下标本身。
    std::vector<std::string> ik_names;
    std::vector<std::string> transform_names;
    std::vector<std::string> path_names;

    // ---- 约束 setup（chunk 18，v11）----
    // 解析顺序 = 文件里的顺序，也就是 `ik_names` / `transform_names` / `path_names` 的序。
    std::vector<SpineSourceIkConstraint> iks;
    std::vector<SpineSourceTransformConstraint> transforms;
    std::vector<SpineSourcePathConstraint> paths;

    // PathAttachment 几何（chunk 20，v11）。**不产生绘制项**，只给 path 约束用。
    std::vector<SpineSourcePathAttachment> path_attachments;

    // flags 来源
    bool has_draw_order_timeline = false;   // animations.*.drawOrder 存在 ⇒ 整个资产走 CPU
    bool uses_tint_black = false;           // 任一槽位带 dark
    bool has_clipping = false;              // 存在 clipping 附件（= !clips.empty()）
    bool has_event_timeline = false;        // 有 event 时间轴（**不烘焙**，但要说出来）

    // 图集页名（写进容器 chunk 7；序列化时才知道纹理引用）
    std::vector<std::string> atlas_pages;

    // ---- 源版本 ----
    SpineSourceVersion version = SpineSourceVersion::kUnknown;
    // 原始版本串（"3.7.83"）。**写实**：容器的 `spine_runtime` 由它算出，
    // 不把 3.7 的资产伪装成 30800 —— 伪装会让"为什么这个资产要用 3.7 的暗色判据"
    // 变成一个查不出来的问题。
    std::string version_string;
};

// 从版本串判定可导入性。只认 3.7.x / 3.8.x；其它一律拒绝（不静默降级）。
// 返回 ok 时 `out_version` / `out_runtime` 已填好（`spine_runtime` 的编码：
// 3.7.xx → 30700、3.8.xx → 30800，与 `SpineGpuConstants::kSpineRuntime37/38` 一致）。
bool ClassifySpineVersion(const std::string& version_string, SpineSourceVersion& out_version,
                          uint32_t& out_runtime, std::string& out_error);

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

    // ---- 影响流（chunk 21 / 22）：weighted 顶点的蒙皮数据（v7 起）----
    // 两条流**共用同一个条目下标** —— 就是 SpineWeightedVertex::influence_offset。
    // 条目总数 = influence_bind.size() / 2 = influence_bone.size()，
    // 读端会校验这个等式（对不上 = 两条流被配错了对，静默错形状）。
    //
    // 为什么搬出顶点：定长 4 槽装不下实测的影响数（最大 15），定长 16 槽又要
    // 176 B/顶点。搬出来后顶点恒 8 B，均值 ≈ 34 B/顶点，shader 循环 2.2 次。
    std::vector<float> influence_bind;              // chunk 21：float2 × 条目数
    std::vector<SpineInfluenceBone> influence_bone; // chunk 22：u16 boneIdx + u8 weight

    std::vector<uint16_t> index_buffer;
    std::vector<SpineDrawItem> draw_items;
    std::vector<uint16_t> slot_bone;          // 每槽位的骨骼下标（rigid 用）

    // 槽位 setup（chunk 17，40 B/槽）。下标 = header.slot_count 的序
    // = chunk 1 里槽位名的序号 = 时间轴 `target`（attachment / color / twoColor）
    // 引用的同一个下标空间。
    // ⚠️ 三处必须同一个下标空间 —— 分叉了不报错，只是颜色/附件挂到别的槽位上。
    std::vector<SpineGpuSlot> slots;

    // 骨骼 setup（chunk 10，36 B/骨）。下标 = header.bone_count 的序，
    // 也 = chunk 1 里骨骼名的序号，也 = 影响流里 bone_idx 引用的同一个下标。
    // ⚠️ 三处必须是同一个下标空间 —— 分叉了不报错，只是蒙皮取到别人的骨骼。
    //
    // **顺序不变量 parent < index**：正向 for 一遍就是拓扑序。
    std::vector<SpineGpuBone> bones;

    // 字符串表（chunk 1）。落盘顺序固定：骨骼名 → 皮肤名 → 槽位名 → 附件名 → 图集页名，
    // 每项以 '\0' 结尾。读回时按 bone_count / skin_count / slot_count / 附件记录数切分。
    std::vector<std::string> bone_names;
    std::vector<std::string> skin_names;
    std::vector<std::string> slot_names;
    std::vector<std::string> attachment_names;
    std::vector<std::string> atlas_page_names;

    // 裁剪多边形记录（chunk 9，12 B/条）。按 (皮肤, 槽位) 排，
    // 下标就是绘制项 `clip_index` 引用的行号。**顶点本身在 vertex_buffer 里**。
    std::vector<SpineGpuClipPolygon> clip_polygons;

    // 附件记录（chunk 6）。Bake 时建好；序列化 / 反序列化都走它 ——
    // 不再从 draw_items 反推（那会假设"附件与绘制项一一对应"，是个脆假设）。
    std::vector<SpineGpuAttachmentRecord> attachment_records;

    // ---- 动画（chunk 11-16，v9）----
    // 四块定长 + 一块变长，相互之间全部是**下标**引用，没有指针 / 没有偏移推导：
    //   anims[].timeline_first  → timelines[]          （条）
    //   timelines[].frame_first → frames[]             （帧，不是 float）
    //   timelines[].curve_first → curve_types[]        （段）
    //   timelines[].bezier_first→ beziers[]            （条）
    //   timelines[].payload_offset → timeline_payload[]（字节）
    // ⚠️ 每一条的单位都不一样，写错一个"帧 vs float"在偏移 0 处逐字节相同。
    std::vector<SpineGpuAnim> anims;
    std::vector<SpineGpuTimeline> timelines;
    std::vector<float> frames;
    std::vector<uint8_t> curve_types;
    std::vector<SpineGpuBezier> beziers;
    std::vector<uint8_t> timeline_payload;

    // 槽位的 setup 附件名（chunk 1，紧跟槽位名之后；条数 = header.slot_count）。
    // 空串 = 这个槽位默认不挂附件 ⇒ chunk 17 里写 `kNoAttachmentName`。
    // 这是 attachment 时间轴的**复位基线**（`AttachmentTimeline.cpp:65-68`）。
    std::vector<std::string> slot_setup_attachment_names;

    // 动画名（chunk 1，排在槽位名 / 附件名 / 图集页名**之后**）。
    // 数量 = anims.size()，读端按 chunk 11 的大小反推。
    std::vector<std::string> anim_names;
    // attachment 时间轴用到的附件名（chunk 1 的最末尾一组）。
    // **只按偏移访问**，读端不需要知道它有多少条 ⇒ 不会破坏前面几组的切分。
    std::vector<std::string> anim_attachment_names;

    // ---- 约束 setup（chunk 18，v11）----
    // 下标空间统一：`iks[i]` / `transforms[i]` / `paths[i]` 的 i 就是
    // chunk 19 里 `SpineGpuUpdateStep::index`（配 kind 用）与时间轴里
    // `ik / transform / path` 时间轴的 target 引用的同一个下标。
    std::vector<SpineGpuIkConstraint> iks;
    std::vector<SpineGpuTransformConstraint> transforms;
    std::vector<SpineGpuPathConstraint> paths;
    // 三类约束的 `bones[]` **共用**一段 u16 索引池（长度可变 ⇒ 不能内联）。
    // 每条约束存 (bone_first, bone_count)，与 v7 影响流同一个思路。
    std::vector<uint16_t> constraint_bone_pool;

    // ---- 执行顺序（chunk 19，v11）----
    // `Skeleton::updateCache()` 的结果**原样落盘**，运行时照着遍历。
    std::vector<SpineGpuUpdateStep> update_steps;
    // `_updateCacheReset`：遍历之前要把哪些骨骼复位到 setup（3.7 `Skeleton.cpp:200-211`）。
    // 少复位 ⇒ 上一帧的应用值残留，表现是"动作拖影"，不报错。
    std::vector<uint16_t> update_reset;

    // ---- PathAttachment 几何（chunk 20，v11）----
    // 不产生绘制项，只给 path 约束用（沿它摆骨骼）。
    std::vector<SpineGpuPathAttachment> path_attachments;
    std::vector<uint16_t> path_bone_pool;   // weighted 才有：[n, idx×n]×vertex_count
    std::vector<float> path_vertices;       // rigid [x,y]×n / weighted [x,y,w]×Σn
    std::vector<float> path_lengths;        // = vertex_count / 3 条
    // 一条 path 附件一个名字，**与 path_attachments 一一对应**（不去重）。
    // 运行时在 attachment 时间轴改掉槽位挂的附件后，要按名字重新查是哪条曲线。
    std::vector<std::string> path_attachment_names;
};

SpineImportResult BakeSpineGpuAsset(const SpineSourceSkeleton& src, SpineGpuAsset& out);

// ---------------------------------------------------------------------
// 容器序列化（规格 §9）
// ---------------------------------------------------------------------
// 附件记录 SpineGpuAttachmentRecord 定义在 SpineGpuFormat.h —— 它是容器布局的
// 一部分，C# 端要照抄，放在"布局的唯一权威定义"里才不会两边漂移。

SpineImportResult WriteSpineGpuContainer(const SpineGpuAsset& asset, std::vector<uint8_t>& out);

// 字符串表的落盘顺序与偏移计算。**只有这一处**实现 —— Bake 拿它算 chunk 11/17 的
// name_offset，Write 拿它拼 chunk 1 的字节，解算器拿它把偏移翻译回字符串。
// 三处共用一份实现，才不会出现"写端按 A 顺序、Bake 按 B 顺序算偏移"的静默错位。
//
// 顺序：骨骼 → 皮肤 → 槽位 → 槽位 setup 附件名 → 附件 → 图集页 →
//       动画名 → path 附件名（v11）→ 动画附件名。
// ⚠️ 动画附件名**必须**最后：它是"剩下的全归它"，没有计数 ⇒ 排在它后面就切不出来。
void BuildSpineStringTable(const SpineGpuAsset& a, std::vector<uint8_t>& blob,
                           std::vector<uint32_t>& slot_setup_attachment_offsets,
                           std::vector<uint32_t>& anim_name_offsets,
                           std::vector<uint32_t>& anim_attachment_offsets,
                           std::vector<uint32_t>& path_attachment_name_offsets);
SpineImportResult ReadSpineGpuContainer(const uint8_t* data, size_t len, SpineGpuAsset& out);
