#include "Runtime/Function/Render/Spine/SpineGpuClipper.h"

#include <cmath>
#include <cstring>
#include <string>

// =====================================================================
// 逐行转录的出处（spine-cpp 3.8，G:/OSGame/unity-engine-source/Modules/Spine/spine-cpp）：
//   * Triangulator::triangulate / decompose / isConcave / positiveArea / winding
//                                         Triangulator.cpp:44-300
//   * SkeletonClipping::clipStart / clipEnd / clipTriangles / clip / makeClockwise
//                                         SkeletonClipping.cpp:43-335
//   * 绘制循环里的状态机               MeshGeneration/SpineMesh.h:107-259
//   * VertexAttachment::computeWorldVertices  VertexAttachment.cpp:64-130
// ⚠️ 浮点表达式的**结合顺序**逐字照抄（不许"化简"）—— 同输入逐位一致靠的就是它。
// =====================================================================

namespace
{
    SpineImportResult Ok()
    {
        SpineImportResult r;
        r.ok = true;
        return r;
    }

    SpineImportResult Fail(std::string message)
    {
        SpineImportResult r;
        r.ok = false;
        r.error = std::move(message);
        return r;
    }

    uint32_t U32At(const std::vector<uint8_t>& bytes, size_t at)
    {
        uint32_t v = 0;
        std::memcpy(&v, bytes.data() + at, 4);
        return v;
    }

    float F32At(const std::vector<uint8_t>& bytes, size_t at)
    {
        float v = 0.0f;
        std::memcpy(&v, bytes.data() + at, 4);
        return v;
    }

    // `VertexAttachment::computeWorldVertices(slot, 0, n, out, 0, 2)` 在我们的顶点格式上的等价物：
    //   rigid    —— `vertex_offset` 起 `count` 个顶点（8 B：bindPos），槽位骨骼单矩阵；deform 非空 ⇒ 整体替换
    //   weighted —— 每顶点 (influence_offset, influence_count)，权重是 **u8 / 255**（容器的量化，
    //               与 spine_gpu.vert 同一个公式 —— 不是参考实现的 float 权重）；deform 按影响顺序逐个加
    // 错误信息的前缀。**不在调用点拼字符串**：那等于每帧每个被裁剪的槽位一次堆分配
    // （`"clip: 绘制项 " + std::to_string(...)` 超过 SSO 容量，必走堆）。改成只带前缀 + 编号，
    // 真的失败时才拼。
    struct ClipLabel
    {
        const char* prefix = "";
        uint32_t id = 0;
        std::string Text() const { return std::string(prefix) + std::to_string(id); }
    };

    SpineImportResult SkinVertices(const SpineGpuAsset& asset, const SpineGpuPoseResult& pose, SpineVertexFormat format,
                                   uint32_t vertex_offset, uint32_t count, uint32_t slot_bone,
                                   const std::vector<float>& deform, std::vector<float>& out, const ClipLabel& what)
    {
        out.assign(static_cast<size_t>(count) * 2u, 0.0f);
        if (format == SpineVertexFormat::kRigid)
        {
            if ((static_cast<size_t>(vertex_offset) + count) * SpineGpuConstants::kRigidVertexStride >
                asset.weighted_segment_offset)
            {
                return Fail(what.Text() + "的 rigid 顶点区间越过 rigid 段");
            }
            if (!deform.empty() && deform.size() != static_cast<size_t>(count) * 2u)
            {
                return Fail(what.Text() + "的 deform 长度 " + std::to_string(deform.size()) + " ≠ 2 × " +
                            std::to_string(count));
            }
            if (slot_bone >= pose.bones.size())
            {
                return Fail(what.Text() + "的槽位骨骼越界");
            }
            const SpineGpuBoneSample& b = pose.bones[slot_bone];
            const float x = b.world_x;
            const float y = b.world_y;
            const float a = b.a, bb = b.b, c = b.c, d = b.d;
            for (uint32_t v = 0; v < count; ++v)
            {
                float vx = 0.0f;
                float vy = 0.0f;
                if (!deform.empty())
                {
                    vx = deform[static_cast<size_t>(v) * 2u];
                    vy = deform[static_cast<size_t>(v) * 2u + 1u];
                }
                else
                {
                    const size_t at = (static_cast<size_t>(vertex_offset) + v) * SpineGpuConstants::kRigidVertexStride;
                    vx = F32At(asset.vertex_buffer, at);
                    vy = F32At(asset.vertex_buffer, at + 4u);
                }
                out[static_cast<size_t>(v) * 2u] = vx * a + vy * bb + x;
                out[static_cast<size_t>(v) * 2u + 1u] = vx * c + vy * d + y;
            }
            return Ok();
        }

        const size_t w0 = asset.weighted_segment_offset;
        if (w0 + (static_cast<size_t>(vertex_offset) + count) * SpineGpuConstants::kRigidVertexStride >
            asset.vertex_buffer.size())
        {
            return Fail(what.Text() + "的 weighted 顶点区间越过顶点缓冲");
        }
        size_t f = 0;   // deform 下标：按影响顺序，每个影响 2 个（VertexAttachment.cpp:114-127）
        for (uint32_t v = 0; v < count; ++v)
        {
            const size_t at = w0 + (static_cast<size_t>(vertex_offset) + v) * SpineGpuConstants::kRigidVertexStride;
            const uint32_t off = U32At(asset.vertex_buffer, at);
            const uint32_t cnt = U32At(asset.vertex_buffer, at + 4u);
            if (static_cast<uint64_t>(off) + cnt > asset.influence_bone.size() ||
                (static_cast<uint64_t>(off) + cnt) * 2u > asset.influence_bind.size())
            {
                return Fail(what.Text() + "的影响区间越过影响流");
            }
            float wx = 0.0f;
            float wy = 0.0f;
            for (uint32_t k = 0; k < cnt; ++k, f += 2u)
            {
                const SpineInfluenceBone& ib = asset.influence_bone[static_cast<size_t>(off) + k];
                if (ib.bone_idx >= pose.bones.size())
                {
                    return Fail(what.Text() + "的影响骨骼越界");
                }
                const SpineGpuBoneSample& bone = pose.bones[ib.bone_idx];
                float vx = asset.influence_bind[(static_cast<size_t>(off) + k) * 2u];
                float vy = asset.influence_bind[(static_cast<size_t>(off) + k) * 2u + 1u];
                if (!deform.empty())
                {
                    if (f + 1u >= deform.size())
                    {
                        return Fail(what.Text() + "的 deform 比影响数短");
                    }
                    vx = vx + deform[f];
                    vy = vy + deform[f + 1u];
                }
                const float weight = static_cast<float>(ib.weight) / 255.0f;
                wx += (vx * bone.a + vy * bone.b + bone.world_x) * weight;
                wy += (vx * bone.c + vy * bone.d + bone.world_y) * weight;
            }
            out[static_cast<size_t>(v) * 2u] = wx;
            out[static_cast<size_t>(v) * 2u + 1u] = wy;
        }
        if (!deform.empty() && f != deform.size())
        {
            return Fail(what.Text() + "的 deform 长度 " + std::to_string(deform.size()) + " ≠ 2 × 影响数 " +
                        std::to_string(f / 2u));
        }
        return Ok();
    }
}   // namespace

// =====================================================================
// Triangulator
// =====================================================================

bool SpineGpuTriangulator::PositiveArea(float p1x, float p1y, float p2x, float p2y, float p3x, float p3y)
{
    return p1x * (p3y - p2y) + p2x * (p1y - p3y) + p3x * (p2y - p1y) >= 0;
}

int SpineGpuTriangulator::Winding(float p1x, float p1y, float p2x, float p2y, float p3x, float p3y)
{
    float px = p2x - p1x, py = p2y - p1y;
    return p3x * py - p3y * px + px * p1y - p1x * py >= 0 ? 1 : -1;
}

bool SpineGpuTriangulator::IsConcave(int index, int vertex_count, const std::vector<float>& vertices,
                                     const std::vector<int>& indices)
{
    int previous = indices[static_cast<size_t>((vertex_count + index - 1) % vertex_count)] << 1;
    int current = indices[static_cast<size_t>(index)] << 1;
    int next = indices[static_cast<size_t>((index + 1) % vertex_count)] << 1;
    return !PositiveArea(vertices[previous], vertices[previous + 1], vertices[current], vertices[current + 1],
                         vertices[next], vertices[next + 1]);
}

const std::vector<int>& SpineGpuTriangulator::Triangulate(const std::vector<float>& vertices)
{
    size_t vertex_count = vertices.size() >> 1;

    std::vector<int>& indices = m_Indices;
    indices.assign(vertex_count, 0);
    for (size_t i = 0; i < vertex_count; ++i)
    {
        indices[i] = static_cast<int>(i);
    }

    std::vector<uint8_t>& is_concave = m_IsConcave;
    is_concave.assign(vertex_count, 0u);
    for (size_t i = 0, n = vertex_count; i < n; ++i)
    {
        is_concave[i] = IsConcave(static_cast<int>(i), static_cast<int>(vertex_count), vertices, indices) ? 1u : 0u;
    }

    std::vector<int>& triangles = m_Triangles;
    triangles.clear();

    while (vertex_count > 3)
    {
        // Find ear tip.
        size_t previous = vertex_count - 1, i = 0, next = 1;

        // outer:
        while (true)
        {
            bool break_outer = false;   // `goto break_outer`
            if (!is_concave[i])
            {
                int p1 = indices[previous] << 1, p2 = indices[i] << 1, p3 = indices[next] << 1;
                float p1x = vertices[p1], p1y = vertices[p1 + 1];
                float p2x = vertices[p2], p2y = vertices[p2 + 1];
                float p3x = vertices[p3], p3y = vertices[p3 + 1];
                for (size_t ii = (next + 1) % vertex_count; ii != previous; ii = (ii + 1) % vertex_count)
                {
                    if (!is_concave[ii])
                    {
                        continue;
                    }
                    int v = indices[ii] << 1;
                    float vx = vertices[v], vy = vertices[v + 1];
                    if (PositiveArea(p3x, p3y, p1x, p1y, vx, vy))
                    {
                        if (PositiveArea(p1x, p1y, p2x, p2y, vx, vy))
                        {
                            if (PositiveArea(p2x, p2y, p3x, p3y, vx, vy))
                            {
                                break_outer = true;
                                break;
                            }
                        }
                    }
                }
                if (!break_outer)
                {
                    break;
                }
            }
            // break_outer:

            if (next == 0)
            {
                do
                {
                    if (!is_concave[i])
                    {
                        break;
                    }
                    i--;
                } while (i > 0);
                break;
            }

            previous = i;
            i = next;
            next = (next + 1) % vertex_count;
        }

        // Cut ear tip.
        triangles.push_back(indices[(vertex_count + i - 1) % vertex_count]);
        triangles.push_back(indices[i]);
        triangles.push_back(indices[(i + 1) % vertex_count]);
        indices.erase(indices.begin() + static_cast<std::ptrdiff_t>(i));
        is_concave.erase(is_concave.begin() + static_cast<std::ptrdiff_t>(i));
        vertex_count--;

        int previous_index = static_cast<int>((vertex_count + i - 1) % vertex_count);
        int next_index = i == vertex_count ? 0 : static_cast<int>(i);
        is_concave[static_cast<size_t>(previous_index)] =
            IsConcave(previous_index, static_cast<int>(vertex_count), vertices, indices) ? 1u : 0u;
        is_concave[static_cast<size_t>(next_index)] =
            IsConcave(next_index, static_cast<int>(vertex_count), vertices, indices) ? 1u : 0u;
    }

    if (vertex_count == 3)
    {
        triangles.push_back(indices[2]);
        triangles.push_back(indices[0]);
        triangles.push_back(indices[1]);
    }

    return triangles;
}

// 每帧每条裁剪多边形都会走一次 ⇒ 中间缓冲全部跨帧复用（实测原来这段占 geo 段的一块）。
// ⚠️ 只改**缓冲的生命周期**，不动任何判据：合并条件、`ii = 0` 的重扫、末尾删空块，
//    全部照抄参考实现（`Triangulator::decompose`）。
std::vector<std::vector<float>>& SpineGpuTriangulator::Decompose(const std::vector<float>& vertices,
                                                                const std::vector<int>& triangles)
{
    std::vector<std::vector<float>>& convex_polygons = m_ConvexPolygons;
    std::vector<std::vector<int>>& convex_polygons_indices = m_ConvexPolygonsIndices;
    size_t n_poly = 0;   // 已攒下的块数（**不**在开头 clear：内层缓冲要留着复用）

    std::vector<int>& polygon_indices = m_PolygonIndices;
    std::vector<float>& polygon = m_Polygon;
    // ⚠️ 必须清：缓冲跨帧留着，但**内容**不能留 —— 不清的话第一块会把上一帧的最后一块再写一遍
    // （表现就是 clipStart 的凸块数比参考多，且不报错）。
    polygon.clear();
    polygon_indices.clear();
    // 把"正在攒的那一块"落进结果里。用 `assign` 写进**已存在**的元素（容量留着），
    // 不用 `push_back(polygon)` —— 后者每块一次拷贝构造 = 一次分配。
    const auto flush = [&]() {
        if (polygon.empty())
        {
            return;
        }
        if (n_poly >= convex_polygons.size())
        {
            convex_polygons.emplace_back();
            convex_polygons_indices.emplace_back();
        }
        convex_polygons[n_poly].assign(polygon.begin(), polygon.end());
        convex_polygons_indices[n_poly].assign(polygon_indices.begin(), polygon_indices.end());
        ++n_poly;
    };

    // Merge subsequent triangles if they form a triangle fan.
    int fan_base_index = -1, last_winding = 0;
    for (size_t i = 0, n = triangles.size(); i < n; i += 3)
    {
        int t1 = triangles[i] << 1, t2 = triangles[i + 1] << 1, t3 = triangles[i + 2] << 1;
        float x1 = vertices[t1], y1 = vertices[t1 + 1];
        float x2 = vertices[t2], y2 = vertices[t2 + 1];
        float x3 = vertices[t3], y3 = vertices[t3 + 1];

        // If the base of the last triangle is the same as this triangle, check if they form a convex polygon (triangle fan).
        bool merged = false;
        if (fan_base_index == t1)
        {
            size_t o = polygon.size() - 4;
            const std::vector<float>& p = polygon;
            int winding1 = Winding(p[o], p[o + 1], p[o + 2], p[o + 3], x3, y3);
            int winding2 = Winding(x3, y3, p[0], p[1], p[2], p[3]);
            if (winding1 == last_winding && winding2 == last_winding)
            {
                polygon.push_back(x3);
                polygon.push_back(y3);
                polygon_indices.push_back(t3);
                merged = true;
            }
        }

        // Otherwise make this triangle the new base.
        if (!merged)
        {
            flush();   // 原来：`if (!polygon.empty()) convex_polygons.push_back(polygon)`
            polygon.clear();   // 原来：`polygon = {x1, y1, x2, y2, x3, y3}`（初始化列表 ⇒ 一次分配）
            polygon.push_back(x1);
            polygon.push_back(y1);
            polygon.push_back(x2);
            polygon.push_back(y2);
            polygon.push_back(x3);
            polygon.push_back(y3);
            polygon_indices.clear();   // 原来：`polygon_indices = {t1, t2, t3}`
            polygon_indices.push_back(t1);
            polygon_indices.push_back(t2);
            polygon_indices.push_back(t3);
            last_winding = Winding(x1, y1, x2, y2, x3, y3);
            fan_base_index = t1;
        }
    }

    flush();

    // Go through the list of polygons and try to merge the remaining triangles with the found triangle fans.
    for (size_t i = 0, n = n_poly; i < n; ++i)
    {
        std::vector<int>& pi = convex_polygons_indices[i];
        if (pi.empty())
        {
            continue;
        }
        int first_index = pi[0];
        int last_index = pi[pi.size() - 1];   // ⚠️ 参考实现在合并后**不更新**它（照抄）

        std::vector<float>& p = convex_polygons[i];
        size_t o = p.size() - 4;
        float prev_prev_x = p[o], prev_prev_y = p[o + 1];
        float prev_x = p[o + 2], prev_y = p[o + 3];
        float first_x = p[0], first_y = p[1];
        float second_x = p[2], second_y = p[3];
        int winding0 = Winding(prev_prev_x, prev_prev_y, prev_x, prev_y, first_x, first_y);

        for (size_t ii = 0; ii < n; ++ii)
        {
            if (ii == i)
            {
                continue;
            }
            std::vector<int>& other_indices = convex_polygons_indices[ii];
            if (other_indices.size() != 3)
            {
                continue;
            }
            int other_first_index = other_indices[0];
            int other_second_index = other_indices[1];
            int other_last_index = other_indices[2];

            std::vector<float>& other_poly = convex_polygons[ii];
            float x3 = other_poly[other_poly.size() - 2], y3 = other_poly[other_poly.size() - 1];

            if (other_first_index != first_index || other_second_index != last_index)
            {
                continue;
            }

            int winding1 = Winding(prev_prev_x, prev_prev_y, prev_x, prev_y, x3, y3);
            int winding2 = Winding(x3, y3, first_x, first_y, second_x, second_y);
            if (winding1 == winding0 && winding2 == winding0)
            {
                other_poly.clear();
                other_indices.clear();
                p.push_back(x3);
                p.push_back(y3);
                pi.push_back(other_last_index);
                prev_prev_x = prev_x;
                prev_prev_y = prev_y;
                prev_x = x3;
                prev_y = y3;
                ii = 0;   // ⚠️ 随后 ++ii ⇒ 从 1 开始重扫（参考实现就是这样，第 0 块被跳过）
            }
        }
    }

    // Remove empty polygons that resulted from the merge step above.
    // 原来是 `erase`（把整条 vector 往左搬 + 析构被删元素 ⇒ 内层缓冲全丢）。
    // 改成"空块换到末尾"再一次性截尾：不搬动，且被截掉的正是空块（本来就没内容）。
    size_t k = 0;
    for (size_t i = 0; i < n_poly; ++i)
    {
        if (convex_polygons[i].empty())
        {
            continue;
        }
        if (k != i)
        {
            convex_polygons[k].swap(convex_polygons[i]);
            convex_polygons_indices[k].swap(convex_polygons_indices[i]);
        }
        ++k;
    }
    // ⚠️ 截尾会析构那几块（缓冲随之释放）。空块没有内容可留，下一帧 `flush()` 会
    //    `emplace_back()` 补回来 ⇒ 稳态每帧最多"上一帧的空块数"次分配。实测为 0。
    convex_polygons.resize(k);
    convex_polygons_indices.resize(k);

    return convex_polygons;
}

// =====================================================================
// SkeletonClipping
// =====================================================================

size_t SpineGpuSkeletonClipping::ClipStart(uint32_t clip_row, uint16_t end_slot, const std::vector<float>& world_polygon)
{
    if (m_Active)
    {
        return 0;
    }
    m_Active = true;
    m_ClipRow = clip_row;
    m_EndSlot = end_slot;

    m_ClippingPolygon = world_polygon;
    if (m_ClippingPolygon.size() < 6)
    {
        // 参考实现在这里 UB（makeClockwise 的 `len - 3` 下溢），见文件头。作用域照常，裁出来为空。
        m_ClippingPolygons = nullptr;
        return 0;
    }
    MakeClockwise(m_ClippingPolygon);
    // **不拷贝**：结果只活在三角化器里，这里只记住指针。
    // 它是唯一的存放处 ⇒ `Decompose()` 下一帧能直接复用上一帧那些块的缓冲（稳态 0 分配）。
    // （拷一份出来的话，`ClipEnd()` 每帧清一次 ⇒ 拷贝必然活不过一帧，等于每帧重新分配。）
    m_ClippingPolygons = &m_Triangulator.Decompose(m_ClippingPolygon, m_Triangulator.Triangulate(m_ClippingPolygon));

    for (std::vector<float>& polygon : *m_ClippingPolygons)
    {
        MakeClockwise(polygon);
        const float x0 = polygon[0];
        const float y0 = polygon[1];
        polygon.push_back(x0);
        polygon.push_back(y0);
    }
    return m_ClippingPolygons->size();
}

void SpineGpuSkeletonClipping::ClipEnd(uint32_t slot)
{
    if (m_Active && m_EndSlot != SpineGpuConstants::kNoClipEndSlot && m_EndSlot == slot)
    {
        ClipEnd();
    }
}

void SpineGpuSkeletonClipping::ClipEnd()
{
    if (!m_Active)
    {
        return;
    }
    m_Active = false;
    m_ClipRow = 0;
    m_EndSlot = SpineGpuConstants::kNoClipEndSlot;
    m_ClippingPolygons = nullptr;   // 只是不再指向三角化器的结果；那些缓冲留给下一帧复用
    m_ClippedVertices.clear();
    m_ClippedUVs.clear();
    m_ClippedTriangles.clear();
    m_ClippingPolygon.clear();
}

void SpineGpuSkeletonClipping::ClipTriangles(const float* vertices, const uint16_t* triangles, size_t triangles_length,
                                             const float* uvs, size_t stride)
{
    std::vector<float>& clip_output = m_ClipOutput;
    std::vector<float>& clipped_vertices = m_ClippedVertices;
    std::vector<uint16_t>& clipped_triangles = m_ClippedTriangles;
    if (m_ClippingPolygons == nullptr)
    {
        return;   // 没在裁（`ClipTriangles` 只在 `IsClipping()` 时被调，这里是兜底）
    }
    const std::vector<std::vector<float>>& polygons = *m_ClippingPolygons;
    const size_t polygons_count = polygons.size();

    size_t index = 0;
    clipped_vertices.clear();
    m_ClippedUVs.clear();
    clipped_triangles.clear();

    size_t i = 0;
    // `continue_outer:` + `for (; i < trianglesLength; i += 3)` —— goto 回到循环头时**不**再加 3
    while (i < triangles_length)
    {
        size_t vertex_offset = triangles[i] * stride;
        float x1 = vertices[vertex_offset], y1 = vertices[vertex_offset + 1];
        float u1 = uvs[vertex_offset], v1 = uvs[vertex_offset + 1];

        vertex_offset = triangles[i + 1] * stride;
        float x2 = vertices[vertex_offset], y2 = vertices[vertex_offset + 1];
        float u2 = uvs[vertex_offset], v2 = uvs[vertex_offset + 1];

        vertex_offset = triangles[i + 2] * stride;
        float x3 = vertices[vertex_offset], y3 = vertices[vertex_offset + 1];
        float u3 = uvs[vertex_offset], v3 = uvs[vertex_offset + 1];

        bool goto_continue_outer = false;
        for (size_t p = 0; p < polygons_count; p++)
        {
            size_t s = clipped_vertices.size();
            if (Clip(x1, y1, x2, y2, x3, y3, polygons[p], clip_output))
            {
                size_t clip_output_length = clip_output.size();
                if (clip_output_length == 0)
                {
                    continue;
                }
                float d0 = y2 - y3, d1 = x3 - x2, d2 = x1 - x3, d4 = y3 - y1;
                float d = 1 / (d0 * d2 + d1 * (y1 - y3));

                size_t clip_output_count = clip_output_length >> 1;
                if (clip_output_count < 2)
                {
                    ++degenerate_pieces;   // 参考实现在这里 `3 * (count - 2)` 下溢，见文件头
                    continue;
                }
                clipped_vertices.resize(s + clip_output_count * 2, 0.0f);
                m_ClippedUVs.resize(s + clip_output_count * 2, 0.0f);
                for (size_t ii = 0; ii < clip_output_length; ii += 2)
                {
                    float x = clip_output[ii], y = clip_output[ii + 1];
                    clipped_vertices[s] = x;
                    clipped_vertices[s + 1] = y;
                    float c0 = x - x3, c1 = y - y3;
                    float a = (d0 * c0 + d1 * c1) * d;
                    float b = (d4 * c0 + d2 * c1) * d;
                    float c = 1 - a - b;
                    m_ClippedUVs[s] = u1 * a + u2 * b + u3 * c;
                    m_ClippedUVs[s + 1] = v1 * a + v2 * b + v3 * c;
                    s += 2;
                }

                s = clipped_triangles.size();
                clipped_triangles.resize(s + 3 * (clip_output_count - 2), 0);
                clip_output_count--;
                for (size_t ii = 1; ii < clip_output_count; ii++)
                {
                    clipped_triangles[s] = static_cast<uint16_t>(index);
                    clipped_triangles[s + 1] = static_cast<uint16_t>(index + ii);
                    clipped_triangles[s + 2] = static_cast<uint16_t>(index + ii + 1);
                    s += 3;
                }
                index += clip_output_count + 1;
            }
            else
            {
                clipped_vertices.resize(s + 3 * 2, 0.0f);
                m_ClippedUVs.resize(s + 3 * 2, 0.0f);
                clipped_vertices[s] = x1;
                clipped_vertices[s + 1] = y1;
                clipped_vertices[s + 2] = x2;
                clipped_vertices[s + 3] = y2;
                clipped_vertices[s + 4] = x3;
                clipped_vertices[s + 5] = y3;

                m_ClippedUVs[s] = u1;
                m_ClippedUVs[s + 1] = v1;
                m_ClippedUVs[s + 2] = u2;
                m_ClippedUVs[s + 3] = v2;
                m_ClippedUVs[s + 4] = u3;
                m_ClippedUVs[s + 5] = v3;

                s = clipped_triangles.size();
                clipped_triangles.resize(s + 3, 0);
                clipped_triangles[s] = static_cast<uint16_t>(index);
                clipped_triangles[s + 1] = static_cast<uint16_t>(index + 1);
                clipped_triangles[s + 2] = static_cast<uint16_t>(index + 2);
                index += 3;
                i += 3;
                goto_continue_outer = true;
                break;
            }
        }
        if (!goto_continue_outer)
        {
            i += 3;
        }
    }
}

bool SpineGpuSkeletonClipping::Clip(float x1, float y1, float x2, float y2, float x3, float y3,
                                    const std::vector<float>& clipping_area, std::vector<float>& output_ref)
{
    std::vector<float>* original_output = &output_ref;
    std::vector<float>* output = &output_ref;
    bool clipped = false;

    // Avoid copy at the end.
    std::vector<float>* input;
    if (clipping_area.size() % 4 >= 2)
    {
        input = output;
        output = &m_Scratch;
    }
    else
    {
        input = &m_Scratch;
    }

    input->clear();
    input->push_back(x1);
    input->push_back(y1);
    input->push_back(x2);
    input->push_back(y2);
    input->push_back(x3);
    input->push_back(y3);
    input->push_back(x1);
    input->push_back(y1);
    output->clear();

    const std::vector<float>& clipping_vertices = clipping_area;
    size_t clipping_vertices_last = clipping_area.size() - 4;
    for (size_t i = 0;; i += 2)
    {
        float edge_x = clipping_vertices[i], edge_y = clipping_vertices[i + 1];
        float edge_x2 = clipping_vertices[i + 2], edge_y2 = clipping_vertices[i + 3];
        float delta_x = edge_x - edge_x2, delta_y = edge_y - edge_y2;

        const std::vector<float>& input_vertices = *input;
        size_t input_vertices_length = input->size() - 2, output_start = output->size();
        for (size_t ii = 0; ii < input_vertices_length; ii += 2)
        {
            float input_x = input_vertices[ii], input_y = input_vertices[ii + 1];
            float input_x2 = input_vertices[ii + 2], input_y2 = input_vertices[ii + 3];
            bool side2 = delta_x * (input_y2 - edge_y2) - delta_y * (input_x2 - edge_x2) > 0;
            if (delta_x * (input_y - edge_y2) - delta_y * (input_x - edge_x2) > 0)
            {
                if (side2)
                {
                    // v1 inside, v2 inside
                    output->push_back(input_x2);
                    output->push_back(input_y2);
                    continue;
                }
                // v1 inside, v2 outside
                float c0 = input_y2 - input_y, c2 = input_x2 - input_x;
                float s = c0 * (edge_x2 - edge_x) - c2 * (edge_y2 - edge_y);
                if (std::fabs(s) > 0.000001f)
                {
                    float ua = (c2 * (edge_y - input_y) - c0 * (edge_x - input_x)) / s;
                    output->push_back(edge_x + (edge_x2 - edge_x) * ua);
                    output->push_back(edge_y + (edge_y2 - edge_y) * ua);
                }
                else
                {
                    output->push_back(edge_x);
                    output->push_back(edge_y);
                }
            }
            else if (side2)
            {
                // v1 outside, v2 inside
                float c0 = input_y2 - input_y, c2 = input_x2 - input_x;
                float s = c0 * (edge_x2 - edge_x) - c2 * (edge_y2 - edge_y);
                if (std::fabs(s) > 0.000001f)
                {
                    float ua = (c2 * (edge_y - input_y) - c0 * (edge_x - input_x)) / s;
                    output->push_back(edge_x + (edge_x2 - edge_x) * ua);
                    output->push_back(edge_y + (edge_y2 - edge_y) * ua);
                }
                else
                {
                    output->push_back(edge_x);
                    output->push_back(edge_y);
                }
                output->push_back(input_x2);
                output->push_back(input_y2);
            }
            clipped = true;
        }

        if (output_start == output->size())
        {
            // All edges outside.
            original_output->clear();
            return true;
        }

        const float ox = (*output)[0];
        const float oy = (*output)[1];
        output->push_back(ox);
        output->push_back(oy);

        if (i == clipping_vertices_last)
        {
            break;
        }
        std::vector<float>* temp = output;
        output = input;
        output->clear();
        input = temp;
    }

    if (original_output != output)
    {
        original_output->clear();
        for (size_t i = 0, n = output->size() - 2; i < n; ++i)
        {
            original_output->push_back((*output)[i]);
        }
    }
    else
    {
        original_output->resize(original_output->size() - 2, 0.0f);
    }

    return clipped;
}

void SpineGpuSkeletonClipping::MakeClockwise(std::vector<float>& polygon)
{
    size_t vertices_length = polygon.size();

    float area = polygon[vertices_length - 2] * polygon[1] - polygon[0] * polygon[vertices_length - 1], p1x, p1y, p2x, p2y;

    for (size_t i = 0, n = vertices_length - 3; i < n; i += 2)
    {
        p1x = polygon[i];
        p1y = polygon[i + 1];
        p2x = polygon[i + 2];
        p2y = polygon[i + 3];
        area += p1x * p2y - p2x * p1y;
    }

    if (area < 0)
    {
        return;
    }

    for (size_t i = 0, last_x = vertices_length - 2, n = vertices_length >> 1; i < n; i += 2)
    {
        float x = polygon[i], y = polygon[i + 1];
        size_t other = last_x - i;
        polygon[i] = polygon[other];
        polygon[i + 1] = polygon[other + 1];
        polygon[other] = x;
        polygon[other + 1] = y;
    }
}

// =====================================================================
// 几何 + 一帧的状态机
// =====================================================================

SpineImportResult SpineGpuComputeItemGeometry(const SpineGpuAsset& asset, const SpineGpuPoseResult& pose,
                                              uint32_t draw_item, const std::vector<float>& deform,
                                              std::vector<float>& world, std::vector<float>& uvs,
                                              std::vector<uint16_t>& triangles)
{
    if (draw_item >= asset.draw_items.size())
    {
        return Fail("clip: 绘制项 " + std::to_string(draw_item) + " 越界");
    }
    const SpineDrawItem& di = asset.draw_items[draw_item];
    if (di.slot_index >= asset.slot_bone.size())
    {
        return Fail("clip: 绘制项 " + std::to_string(draw_item) + " 的槽位越界");
    }
    // 这个函数**每个被裁剪的槽位每帧都会走一次**。原来它有 4 处每帧堆分配：两个局部
    // `std::vector`、`triangles = {…}` 的初始化列表、以及拼错误信息的 `std::string`。
    // 中间缓冲改成 `thread_local` 跨帧复用，错误信息改成失败时才拼 ⇒ 稳态 0 分配。
    thread_local std::vector<float> t_item_world;
    thread_local std::vector<float> t_item_uv;
    const ClipLabel what{"clip: 绘制项 ", draw_item};
    std::vector<float>& local_world = t_item_world;
    std::vector<float>& local_uv = t_item_uv;
    SpineImportResult r = SkinVertices(asset, pose, di.vertex_format, di.vertex_base, di.vertex_count,
                                       asset.slot_bone[di.slot_index], deform, local_world, what);
    if (!r.ok)
    {
        return r;
    }
    local_uv.assign(static_cast<size_t>(di.vertex_count) * 2u, 0.0f);
    for (uint32_t v = 0; v < di.vertex_count; ++v)
    {
        const size_t uv = static_cast<size_t>(static_cast<uint32_t>(di.uv_offset + di.vertex_base + v)) * 2u;
        if (uv + 1u >= asset.uv_buffer.size())
        {
            return Fail(what.Text() + " 的 UV 越界");
        }
        local_uv[static_cast<size_t>(v) * 2u] = asset.uv_buffer[uv];
        local_uv[static_cast<size_t>(v) * 2u + 1u] = asset.uv_buffer[uv + 1u];
    }
    if (static_cast<uint64_t>(di.index_offset) + di.index_count > asset.index_buffer.size())
    {
        return Fail(what.Text() + " 的索引区间越界");
    }

    if ((di.item_flags & SpineDrawItemFlags::kRegion) != 0)
    {
        // 参考喂给 clipTriangles 的是 BR, BL, UL, UR + regionTriangles {0,1,2, 2,3,0}
        // （RegionAttachment.cpp:137-158、SpineMesh.cpp:74-80）。我们存的是 BL, UL, UR, BR。
        if (di.vertex_count != 4)
        {
            return Fail(what.Text() + " 标成 region 却不是 4 个顶点");
        }
        static const uint32_t kOurOfRef[4] = {3, 0, 1, 2};
        static const uint16_t kRegionTris[6] = {0, 1, 2, 2, 3, 0};
        world.assign(8, 0.0f);
        uvs.assign(8, 0.0f);
        for (uint32_t k = 0; k < 4; ++k)
        {
            world[k * 2u] = local_world[kOurOfRef[k] * 2u];
            world[k * 2u + 1u] = local_world[kOurOfRef[k] * 2u + 1u];
            uvs[k * 2u] = local_uv[kOurOfRef[k] * 2u];
            uvs[k * 2u + 1u] = local_uv[kOurOfRef[k] * 2u + 1u];
        }
        triangles.assign(kRegionTris, kRegionTris + 6);
        return Ok();
    }

    // mesh：把结果搬进输出缓冲。用 `swap` 而不是 `world = std::move(local_world)`：后者会把
    // world 的旧缓冲丢掉（下一帧又得重新分配），swap 让两块缓冲轮流复用 ⇒ 稳态 0 分配。
    world.swap(local_world);
    uvs.swap(local_uv);
    triangles.resize(di.index_count);
    for (uint32_t q = 0; q < di.index_count; ++q)
    {
        const uint32_t idx = asset.index_buffer[static_cast<size_t>(di.index_offset) + q];
        if (idx < di.vertex_base || idx >= static_cast<uint32_t>(di.vertex_base) + di.vertex_count)
        {
            return Fail(what.Text() + " 的索引不在它的顶点区间里");
        }
        triangles[q] = static_cast<uint16_t>(idx - di.vertex_base);
    }
    return Ok();
}

SpineImportResult SpineGpuComputeClipPolygonWorld(const SpineGpuAsset& asset, const SpineGpuPoseResult& pose,
                                                  uint32_t clip_row, const std::vector<float>& deform,
                                                  std::vector<float>& world)
{
    if (clip_row >= asset.clip_polygons.size())
    {
        return Fail("clip: 裁剪多边形 " + std::to_string(clip_row) + " 越界");
    }
    const SpineGpuClipPolygon& c = asset.clip_polygons[clip_row];
    if (c.slot_index >= asset.slot_bone.size())
    {
        return Fail("clip: 裁剪多边形 " + std::to_string(clip_row) + " 的槽位越界");
    }
    const ClipLabel what{"clip: 裁剪多边形 ", clip_row};
    return SkinVertices(asset, pose, c.vertex_format, c.vertex_offset, c.vertex_count, asset.slot_bone[c.slot_index],
                        deform, world, what);
}

SpineImportResult SpineGpuClipFrame(const SpineGpuAsset& asset, const SpineGpuSkeletonState& state,
                                    const SpineGpuPoseResult& pose, SpineGpuSkeletonClipping& clipping,
                                    SpineGpuClipFrameResult& out)
{
    out.draws.clear();
    out.xy.clear();
    out.uv.clear();
    out.indices.clear();
    out.slot_clipped.assign(state.slots.size(), SpineGpuClipFrameResult::kNotClipped);
    out.clip_starts = 0;
    out.clip_pieces = 0;
    clipping.ClipEnd();
    if (asset.clip_polygons.empty())
    {
        return Ok();   // 没有裁剪附件 ⇒ instruction.hasClipping 恒假，什么都不用做
    }
    if (pose.slot_draw_item.size() != state.slots.size())
    {
        return Fail("clip: pose 与状态的槽位数不一致");
    }

    // 每帧的中间缓冲。**不能**是函数内的局部 `std::vector`：那等于每帧 3 次 malloc（实测这些
    // 分配全落在 geo 段，是最贵那几个资产 geo 成本的一块）。`thread_local` 跨帧复用。
    thread_local std::vector<float> t_frame_world;
    thread_local std::vector<float> t_frame_uvs;
    thread_local std::vector<uint16_t> t_frame_tris;
    std::vector<float>& world = t_frame_world;
    std::vector<float>& uvs = t_frame_uvs;
    std::vector<uint16_t>& tris = t_frame_tris;
    for (uint32_t slot : pose.draw_order)
    {
        if (slot >= state.slots.size())
        {
            return Fail("clip: 绘制顺序里的槽位 " + std::to_string(slot) + " 越界");
        }
        const SpineGpuSlotState& s = state.slots[slot];
        if (s.attachment.name_id == SpineGpuAttachRef::kNone)
        {
            clipping.ClipEnd(slot);   // attachment == nullptr（SpineMesh.h:121-126）
            continue;
        }
        const uint32_t item = pose.slot_draw_item[slot];
        if (item != SpineGpuPoseResult::kNoDrawItem)
        {
            // region / mesh（:127-151）
            if (clipping.IsClipping())
            {
                SpineImportResult r = SpineGpuComputeItemGeometry(asset, pose, item, s.deform, world, uvs, tris);
                if (!r.ok)
                {
                    return r;
                }
                clipping.ClipTriangles(world.data(), tris.data(), tris.size(), uvs.data(), 2);
                SpineGpuClippedDraw d;
                d.slot = slot;
                d.draw_item = item;
                d.clip_row = clipping.ActiveClipRow();
                d.vertex_first = static_cast<uint32_t>(out.xy.size() / 2u);
                d.vertex_count = static_cast<uint32_t>(clipping.ClippedVertices().size() / 2u);
                d.index_first = static_cast<uint32_t>(out.indices.size());
                d.index_count = static_cast<uint32_t>(clipping.ClippedTriangles().size());
                out.xy.insert(out.xy.end(), clipping.ClippedVertices().begin(), clipping.ClippedVertices().end());
                out.uv.insert(out.uv.end(), clipping.ClippedUVs().begin(), clipping.ClippedUVs().end());
                out.indices.insert(out.indices.end(), clipping.ClippedTriangles().begin(),
                                   clipping.ClippedTriangles().end());
                out.slot_clipped[slot] = static_cast<uint32_t>(out.draws.size());
                out.draws.push_back(d);
            }
            clipping.ClipEnd(slot);   // :257
            continue;
        }
        if (s.attachment.clip != SpineGpuAttachRef::kNone)
        {
            // ClippingAttachment（:152-160）：clipStart 后**直接 continue**（不调 clipEnd(slot)）
            if (!clipping.IsClipping())
            {
                const uint32_t row = s.attachment.clip;
                SpineImportResult r = SpineGpuComputeClipPolygonWorld(asset, pose, row, s.deform, world);
                if (!r.ok)
                {
                    return r;
                }
                out.clip_pieces += static_cast<uint32_t>(clipping.ClipStart(row, asset.clip_polygons[row].end_slot, world));
                ++out.clip_starts;
            }
            continue;
        }
        clipping.ClipEnd(slot);   // 其它（path / bbox / point / 找不到）（:161-165）
    }
    clipping.ClipEnd();   // :259
    return Ok();
}
