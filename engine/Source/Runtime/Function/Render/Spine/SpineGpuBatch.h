#pragma once

// =====================================================================
// GPU-Spine 多实例拼包（规格 doc/GPU_SPINE_INTERCHANGE_FORMAT.md §15 "多实例"）
// ---------------------------------------------------------------------
// 同一个资产的所有实例**共用**静态缓冲（顶点 / 影响 / UV / 静态索引 / 图集页），每帧把各实例的
// 解算结果首尾相接拼进同一组每帧缓冲，着色器按 `SpineDraw` 里的偏置寻址：
//
//   b4 bones     —— 实例 k 的骨骼表从 `bone_base`（骨下标）开始；
//   b5 draws     —— 实例 k 的绘制项是一段连续区间，firstInstance = 绘制项在 b5 里的**全局**下标；
//   b6 insts     —— `inst_index` 指向实例 k 的变换（clip_from_world + root）；
//   b7 deform    —— 实例 k 的 deform 段拼在前面实例之后，`deform_base` 已含该偏移；
//   b8 cpu_verts —— 实例 k 的裁剪顶点同上，`deform_base`（bit3）已含偏移；
//   cpu_index    —— 局部于各项的 u16 下标，命令的 `index_first` 已含该实例在动态索引缓冲里的偏移。
//
// 这里**只做 CPU 侧的拼包**（不碰 RHI），所以能在没有 GPU 的单测 / 全量对拍里验证：
// `SpineGpuEmulateVertex()` 是 spine_gpu.vert 的逐行 CPU 版（同一运算顺序、越界即报错），
// "实例 k 在合包里的每个顶点 == 它单独成包时的同一个顶点"逐位成立，偏置就没算错。
// 着色器侧（真的按 bone_base / inst_index 寻址）由 SpineGpuDx12RasterTest 在 GPU 上验。
// =====================================================================

#include "Runtime/Function/Render/Spine/SpineGpuClipper.h"
#include "Runtime/Function/Render/Spine/SpineGpuFormat.h"
#include "Runtime/Function/Render/Spine/SpineGpuImporter.h"
#include "Runtime/Function/Render/Spine/SpineGpuPoseSolver.h"

#include <cstdint>
#include <vector>

// 着色器里的两个 std430 结构，**逐字段**与 spine_gpu.vert / .frag（以及 .hlsl）对齐。
// 尺寸钉成 static_assert：改了一边没改另一边时是"能跑、读到别人的字段"，不报错。
struct SpineGpuGpuDraw
{
    uint32_t vertex_format;
    uint32_t uv_offset;
    uint32_t slot_bone;
    uint32_t bone_base;     // 该实例骨骼表在 b4 里的起始骨下标
    uint32_t inst_index;    // 进 b6
    uint32_t tint;
    uint32_t flags;
    uint32_t weighted_base;
    uint32_t deform_base;   // bit2：b7 的回绕偏置；bit3：b8（CPU 裁剪顶点）里的起点
    float dark[3];          // bit4：槽位暗色 rgb（float，不截断；见 SpineGpuTintBlack.h）
};
static_assert(sizeof(SpineGpuGpuDraw) == 48, "spine_gpu.vert SpineDraw is 12 x u32 (std430)");

struct SpineGpuGpuInst
{
    float clip_from_world[16];   // 列主序（GLSL mat4 默认）
    float root_abcd[4];
    float root_t[4];
};
static_assert(sizeof(SpineGpuGpuInst) == 96, "spine_gpu.vert SpineInst is mat4 + vec4 + vec4 (std430)");

// CPU 裁剪顶点（b8）的一项：骨架空间位置 + UV，与 spine_gpu.vert 的 vec4 对齐
struct SpineGpuCpuVert
{
    float x, y, u, v;
};
static_assert(sizeof(SpineGpuCpuVert) == 16, "spine_gpu.vert cpu_verts is vec4");

namespace SpineGpuDrawFlags
{
    constexpr uint32_t kStraightAlpha = 1u;
    constexpr uint32_t kAdditive = 2u;
    constexpr uint32_t kDeform = 4u;
    constexpr uint32_t kCpuVerts = 8u;     // v15：CPU 裁剪过的项，顶点取 b8
    constexpr uint32_t kTintBlack = 16u;   // tint black：片元加暗色项（spine_gpu.frag）
}

// unpackUnorm4x8：x 在最低字节；分量先夹到 [0, 1]
uint32_t SpineGpuPackRgba8(const float c[4]);

// 每个资产算一次：每个绘制项的 deform 数组长度（float 个数；rigid 2×顶点、weighted 2×影响）、
// weighted 项的 infl_base（第 0 个顶点的 influence_offset）—— GPU 寻址偏置要用；以及单实例
// 一帧内 deform 的上界（所有 default 皮肤 kFallback 项同时生效）。
struct SpineGpuBatchAssetInfo
{
    std::vector<uint32_t> deform_len;
    std::vector<uint32_t> infl_base;
    size_t deform_capacity = 0;
};
void SpineGpuBuildBatchAssetInfo(const SpineGpuAsset& asset, SpineGpuBatchAssetInfo& out);

// 一个绘制命令：静态项按容器的索引区间画；裁剪过的项按动态索引缓冲画
struct SpineGpuBatchCmd
{
    uint32_t item = 0;
    bool cpu = false;   // true = 动态索引缓冲（cpu_index）
    uint32_t index_first = 0;
    uint32_t index_count = 0;
};

// 一个实例在合包里占的区间与本帧统计
struct SpineGpuBatchRange
{
    uint32_t draw_first = 0;   // 进 draws / cmds
    uint32_t draw_count = 0;
    uint32_t deformed = 0;
    uint32_t clipped = 0;
    uint32_t tint_black = 0;
    uint32_t deform_floats = 0;
    uint32_t clip_vertices = 0;
};

// 一个资产这一帧的合包（多个实例首尾相接）
struct SpineGpuBatch
{
    std::vector<SpineGpuBoneSample> bones;   // b4
    std::vector<SpineGpuGpuDraw> draws;      // b5
    std::vector<SpineGpuBatchCmd> cmds;      // 与 draws 一一对应
    std::vector<SpineGpuGpuInst> insts;      // b6
    std::vector<float> deform;               // b7
    std::vector<SpineGpuCpuVert> cpu_verts;  // b8
    std::vector<uint16_t> cpu_index;         // 动态索引缓冲
    std::vector<int64_t> scratch_region;     // 内部暂存：绘制项 → 本实例 deform 段起点

    void Clear();
};

// 把一个实例追加进合包：骨骼 / 本帧可见绘制项（按绘制顺序，取槽位当前附件）/ deform / 裁剪结果 / 变换。
// `clip` 可为空（= 本帧没跑裁剪 ⇒ 全部 GPU 蒙皮）。失败时合包**原样回滚**（别的实例照画）。
// 失败只可能是"解算结果与资产不自洽"（deform 长度不符 / 超出单实例上界 / 下标越界）。
SpineImportResult SpineGpuAppendBatchInstance(const SpineGpuAsset& asset, const SpineGpuBatchAssetInfo& info,
                                              const SpineGpuPoseResult& pose, const SpineGpuClipFrameResult* clip,
                                              const SpineGpuGpuInst& inst, bool tint_black_on, SpineGpuBatch& batch,
                                              SpineGpuBatchRange& range);

// spine_gpu.vert 的 CPU 版：合包里第 `draw` 个绘制项的第 `vi` 个顶点（= gl_VertexIndex，
// 静态项是容器里的绝对顶点下标、裁剪项是局部下标）→ 裁剪空间 xy（w 恒为 1）+ UV。
// 任何一次寻址越界 ⇒ 返回 false（GPU 上那是"读到别人的数据、不报错"）。
struct SpineGpuEmulatedVertex
{
    float clip_x = 0.0f;
    float clip_y = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
};
bool SpineGpuEmulateVertex(const SpineGpuAsset& asset, const SpineGpuBatch& batch, uint32_t draw, uint32_t vi,
                           SpineGpuEmulatedVertex& out);

// ---- 摆放 ----
// 视口像素矩形（原点左上、y 向下）
struct SpineGpuRect
{
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

// 自动排布：`count` 个实例铺满视口的网格里第 `index` 格。列数 ≈ sqrt(count · 宽高比)，
// 行从上往下排。count == 1 ⇒ 整个视口（= 多实例之前的单实例画法）。
SpineGpuRect SpineGpuGridCell(uint32_t count, uint32_t index, float viewport_w, float viewport_h);

// 把 setup 包围盒 `bounds`（x0, y0, x1, y1，骨架空间 y 向上）居中放进 `cell`、占 80%。
// `y_sign`：裁剪空间 y 的方向（Vulkan -1：NDC y 向下；DX12 / Metal +1）。
SpineGpuGpuInst SpineGpuFitInstance(const float bounds[4], const SpineGpuRect& cell, float viewport_w,
                                    float viewport_h, float y_sign);

// 手动摆放：骨架原点放在视口中心偏 (x, y) 像素处（y 向上），1 骨架单位 = `scale` 像素。
SpineGpuGpuInst SpineGpuPlaceInstance(float x, float y, float scale, float viewport_w, float viewport_h,
                                      float y_sign);
