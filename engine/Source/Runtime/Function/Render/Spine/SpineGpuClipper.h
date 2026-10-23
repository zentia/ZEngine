#pragma once

// =====================================================================
// GPU-Spine 的 CPU 裁剪（规格 doc/GPU_SPINE_INTERCHANGE_FORMAT.md §7.2，
// doc/GPU_SPINE_V7_ANIMATION.md §7.0.8）
// ---------------------------------------------------------------------
// 3.8 的裁剪只有 CPU 实现：`SkeletonClipping::clipTriangles()` 把被裁附件的三角形逐个与
// 裁剪多边形（凸分解后的若干块）求交，**产生新的顶点 / 三角形 / UV** ⇒ 顶点着色器做不了。
// 所以每帧：
//   1. 按**当前**绘制顺序跑一遍参考实现的作用域状态机（MeshGeneration/SpineMesh.h:107-259）；
//   2. 作用域内的可画项：在 CPU 上蒙皮出世界坐标（与 spine_gpu.vert 同一个公式），喂给
//      转录过来的 `SkeletonClipping`，结果上传成该项的 CPU 顶点（b7）+ 动态索引；
//   3. 作用域外的项照旧走 GPU 蒙皮。
//
// 两部分分开写、分开对拍：
//   * `SpineGpuSkeletonClipping` —— `SkeletonClipping` + `Triangulator` 的**逐行转录**
//     （SkeletonClipping.cpp:43-335、Triangulator.cpp:44-300）。同样的输入必须**逐位**
//     得到同样的输出（对拍台喂参考实现的世界坐标，比 clippedVertices / UVs / Triangles）。
//   * `SpineGpuClipFrame()` —— 状态机 + 蒙皮。蒙皮用容器里 u8 量化的权重，所以端到端
//     只能按容差比（weighted 的底噪见 S6）。
//
// 与参考实现**有意不同**的地方（都是参考实现在坏数据上 UB / 崩溃的分支）：
//   * 世界多边形少于 3 个点：参考 `makeClockwise` 的循环上界 `len - 3` 下溢 ⇒ 越界读。
//     这里照样"开始裁剪"（作用域不变），但凸分解结果为空 ⇒ 作用域内什么都不画。
//   * 某块裁剪输出只有 1 个点：参考 `setSize(s + 3 * (count - 2))` 下溢 ⇒ 天文数字分配。
//     这里丢弃这一块（计数在 `degenerate_pieces`）。
//
// 只认 default 皮肤（与绘制项解析一致）；`useClipping` 视为开启（OSGame 1483 个 prefab
// 写 1、176 个写 0 —— 关掉的那部分画面本来就不裁，是宿主的选择，不是数据）。
// =====================================================================

#include "Runtime/Function/Render/Spine/SpineGpuFormat.h"
#include "Runtime/Function/Render/Spine/SpineGpuImporter.h"
#include "Runtime/Function/Render/Spine/SpineGpuPoseSolver.h"
#include "Runtime/Function/Render/Spine/SpineGpuSkeletonState.h"

#include <cstddef>
#include <cstdint>
#include <vector>

// `spine::Triangulator` 的转录（Triangulator.cpp:44-300）。
class SpineGpuTriangulator
{
public:
    // 耳切法三角化；返回**顶点下标**（不是 float 下标）的三元组序列。
    const std::vector<int>& Triangulate(const std::vector<float>& vertices);

    // 把三角形合并成凸多边形（扇形合并 + 补三角形）。返回的每块是 [x, y, ...]。
    //
    // 返回**非 const** 引用：`ClipStart()` 要 `swap` 走结果（每帧拷贝 = 每块一次分配 +
    // 一次释放）。三角形与其凸分解都只在三角化器里活一轮，换出去的旧缓冲下一帧继续用。
    std::vector<std::vector<float>>& Decompose(const std::vector<float>& vertices, const std::vector<int>& triangles);

    static bool PositiveArea(float p1x, float p1y, float p2x, float p2y, float p3x, float p3y);
    static int Winding(float p1x, float p1y, float p2x, float p2y, float p3x, float p3y);

private:
    static bool IsConcave(int index, int vertex_count, const std::vector<float>& vertices, const std::vector<int>& indices);

    std::vector<std::vector<float>> m_ConvexPolygons;
    std::vector<std::vector<int>> m_ConvexPolygonsIndices;
    std::vector<int> m_Indices;
    std::vector<uint8_t> m_IsConcave;
    std::vector<int> m_Triangles;
    // `Decompose()` 里"正在攒的那一块"。原来是函数内的局部 `std::vector` ⇒ 每帧每条裁剪
    // 多边形 2 次分配（还要随扇形合并反复扩容）。跨帧留着就只在第一次分配。
    std::vector<int> m_PolygonIndices;
    std::vector<float> m_Polygon;
};

// `spine::SkeletonClipping` 的转录（SkeletonClipping.cpp:43-335）。
// "裁剪附件"在这里是 (chunk 9 行号, 结束槽位) —— 参考实现比的是 `_endSlot == &slot._data`。
class SpineGpuSkeletonClipping
{
public:
    // `clipStart()`：已经在裁 ⇒ 返回 0 且什么都不改（嵌套被忽略，:49-51）。
    // `world_polygon` = `computeWorldVertices(slot, 0, n, ...)` 的结果（[x, y] × 顶点数）。
    size_t ClipStart(uint32_t clip_row, uint16_t end_slot, const std::vector<float>& world_polygon);
    void ClipEnd(uint32_t slot);   // `clipEnd(Slot&)`：只在 slot == 结束槽位时收尾
    void ClipEnd();                // `clipEnd()`
    bool IsClipping() const { return m_Active; }
    uint32_t ActiveClipRow() const { return m_ClipRow; }

    // `clipTriangles(vertices, triangles, trianglesLength, uvs, stride)`；stride 是"每顶点几个 float"。
    void ClipTriangles(const float* vertices, const uint16_t* triangles, size_t triangles_length, const float* uvs,
                       size_t stride);

    const std::vector<float>& ClippedVertices() const { return m_ClippedVertices; }
    const std::vector<uint16_t>& ClippedTriangles() const { return m_ClippedTriangles; }
    const std::vector<float>& ClippedUVs() const { return m_ClippedUVs; }
    // 凸分解的结果**不拷贝**：直接指向三角化器里那一份（拷贝 = 每帧每块一次分配 + 一次释放，
    // 而 `ClipEnd()` 每帧都会把它清掉 ⇒ 拷贝出来的一定活不过一帧）。
    const std::vector<std::vector<float>>& ClippingPolygons() const
    {
        static const std::vector<std::vector<float>> kEmpty;
        return m_ClippingPolygons != nullptr ? *m_ClippingPolygons : kEmpty;
    }

    size_t degenerate_pieces = 0;   // 被丢弃的"只有 1 个点"的裁剪输出（见文件头）

    static void MakeClockwise(std::vector<float>& polygon);

private:
    bool Clip(float x1, float y1, float x2, float y2, float x3, float y3, const std::vector<float>& clipping_area,
              std::vector<float>& output);

    SpineGpuTriangulator m_Triangulator;
    std::vector<float> m_ClippingPolygon;
    std::vector<float> m_ClipOutput;
    std::vector<float> m_ClippedVertices;
    std::vector<uint16_t> m_ClippedTriangles;
    std::vector<float> m_ClippedUVs;
    std::vector<float> m_Scratch;
    // == nullptr ⇔ 现在不在裁。非空时指向 `m_Triangulator` 里那一份结果（见 `ClippingPolygons()`）。
    // 非 const：`ClipStart()` 要给每块补上闭合点（push_back 首点）。
    std::vector<std::vector<float>>* m_ClippingPolygons = nullptr;
    bool m_Active = false;
    uint32_t m_ClipRow = 0;
    uint16_t m_EndSlot = SpineGpuConstants::kNoClipEndSlot;
};

// 一个被裁的绘制项（按绘制顺序）。顶点 / 索引都是**局部于这一项**的（索引 0 = vertex_first）。
struct SpineGpuClippedDraw
{
    uint32_t slot = 0;
    uint32_t draw_item = 0;
    uint32_t clip_row = 0;       // 当时生效的裁剪多边形（chunk 9 行号）
    uint32_t vertex_first = 0;   // 进 xy / uv（单位：顶点）
    uint32_t vertex_count = 0;
    uint32_t index_first = 0;    // 进 indices
    uint32_t index_count = 0;    // 0 = 整个被裁掉（这一帧不画它）
};

struct SpineGpuClipFrameResult
{
    static constexpr uint32_t kNotClipped = 0xFFFFFFFFu;
    std::vector<SpineGpuClippedDraw> draws;
    std::vector<float> xy;          // 骨架空间（root 变换之前），2 float/顶点
    std::vector<float> uv;          // 2 float/顶点
    std::vector<uint16_t> indices;  // 局部于各自的 vertex_first
    // [slot] → draws 下标；kNotClipped = 不在任何裁剪作用域里（照旧走 GPU 蒙皮）
    std::vector<uint32_t> slot_clipped;
    // 诊断：本帧开始过几次裁剪、多边形凸分解出几块
    uint32_t clip_starts = 0;
    uint32_t clip_pieces = 0;
};

// 绘制项 → 参考实现喂给 `clipTriangles()` 的那三样（世界坐标 / UV / 三角形，附件本地下标）。
// region 按参考的 BR, BL, UL, UR 重排（item_flags & kRegion，见 SpineDrawItemFlags）。
// `deform` = 槽位的 deform 数组（空 = 用 setup 顶点）。
SpineImportResult SpineGpuComputeItemGeometry(const SpineGpuAsset& asset, const SpineGpuPoseResult& pose,
                                              uint32_t draw_item, const std::vector<float>& deform,
                                              std::vector<float>& world, std::vector<float>& uvs,
                                              std::vector<uint16_t>& triangles);

// 裁剪多边形的世界坐标（`ClippingAttachment::computeWorldVertices(slot, 0, n, ...)`）。
SpineImportResult SpineGpuComputeClipPolygonWorld(const SpineGpuAsset& asset, const SpineGpuPoseResult& pose,
                                                  uint32_t clip_row, const std::vector<float>& deform,
                                                  std::vector<float>& world);

// 一帧的裁剪：按 `pose.draw_order` 跑 SpineMesh.h 的状态机。`clipping` 是可复用的暂存。
SpineImportResult SpineGpuClipFrame(const SpineGpuAsset& asset, const SpineGpuSkeletonState& state,
                                    const SpineGpuPoseResult& pose, SpineGpuSkeletonClipping& clipping,
                                    SpineGpuClipFrameResult& out);
