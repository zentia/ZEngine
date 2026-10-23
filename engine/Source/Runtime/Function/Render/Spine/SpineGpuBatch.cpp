// GPU-Spine 多实例拼包 —— 见 SpineGpuBatch.h
#include "Runtime/Function/Render/Spine/SpineGpuBatch.h"

#include "Runtime/Function/Render/Spine/SpineGpuTintBlack.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

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

    uint32_t WordAt(const SpineGpuAsset& a, size_t word)
    {
        uint32_t v = 0;
        std::memcpy(&v, a.vertex_buffer.data() + word * 4u, 4);
        return v;
    }

    void ApplyTintBlack(SpineGpuGpuDraw& g, const SpineGpuSlotSample& slot, bool tint_black_on, bool additive,
                        uint32_t& counter)
    {
        const SpineGpuTintBlackParams tb = SpineGpuMakeTintBlackParams(slot.dark, tint_black_on, additive);
        if (!tb.enabled)
        {
            return;
        }
        g.flags |= SpineGpuDrawFlags::kTintBlack;
        g.dark[0] = tb.dark[0];
        g.dark[1] = tb.dark[1];
        g.dark[2] = tb.dark[2];
        ++counter;
    }

    SpineGpuGpuInst Affine(float sx, float sy, float tx, float ty)
    {
        SpineGpuGpuInst inst {};
        inst.clip_from_world[0] = sx;
        inst.clip_from_world[5] = sy;
        inst.clip_from_world[10] = 1.0f;
        inst.clip_from_world[12] = tx;
        inst.clip_from_world[13] = ty;
        inst.clip_from_world[15] = 1.0f;
        inst.root_abcd[0] = 1.0f;   // 根变换：单位阵（摆放交给 clip_from_world）
        inst.root_abcd[3] = 1.0f;
        return inst;
    }
}   // namespace

uint32_t SpineGpuPackRgba8(const float c[4])
{
    const auto q = [](float v) {
        v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        return static_cast<uint32_t>(std::lround(v * 255.0f));
    };
    return q(c[0]) | (q(c[1]) << 8u) | (q(c[2]) << 16u) | (q(c[3]) << 24u);
}

void SpineGpuBuildBatchAssetInfo(const SpineGpuAsset& a, SpineGpuBatchAssetInfo& out)
{
    out.deform_len.assign(a.draw_items.size(), 0u);
    out.infl_base.assign(a.draw_items.size(), 0u);
    out.deform_capacity = 0;
    // 单实例一帧的 deform 上界：每个槽位同一时刻只挂一条附件 ⇒ Σ槽位 max(该槽位所有皮肤的 kFallback 项)
    // （只有被 deform 时间轴命中过的项才是 kFallback）。换肤之前只看 default 皮肤、按项求和。
    std::vector<uint32_t> slot_max(a.slots.size(), 0u);
    for (uint32_t i = 0; i < a.draw_items.size(); ++i)
    {
        const SpineDrawItem& di = a.draw_items[i];
        if (di.vertex_format == SpineVertexFormat::kRigid)
        {
            out.deform_len[i] = 2u * di.vertex_count;
        }
        else
        {
            const size_t w0 = a.weighted_segment_offset / 4u + static_cast<size_t>(di.vertex_base) * 2u;
            uint32_t n = 0;
            for (uint32_t v = 0; v < di.vertex_count; ++v)
            {
                n += WordAt(a, w0 + v * 2u + 1u);
            }
            out.deform_len[i] = 2u * n;
            if (di.vertex_count > 0)
            {
                out.infl_base[i] = WordAt(a, w0);
            }
        }
        if (di.kind == SpineDrawItemKind::kFallback && di.slot_index < slot_max.size())
        {
            slot_max[di.slot_index] = std::max(slot_max[di.slot_index], out.deform_len[i]);
        }
    }
    for (uint32_t m : slot_max)
    {
        out.deform_capacity += m;
    }
}

void SpineGpuBatch::Clear()
{
    bones.clear();
    draws.clear();
    cmds.clear();
    insts.clear();
    deform.clear();
    cpu_verts.clear();
    cpu_index.clear();
}

SpineImportResult SpineGpuAppendBatchInstance(const SpineGpuAsset& a, const SpineGpuBatchAssetInfo& info,
                                              const SpineGpuPoseResult& pose, const SpineGpuClipFrameResult* clip,
                                              const SpineGpuGpuInst& inst, bool tint_black_on, SpineGpuBatch& batch,
                                              SpineGpuBatchRange& range)
{
    range = SpineGpuBatchRange {};
    if (pose.bones.size() != a.header.bone_count || info.deform_len.size() != a.draw_items.size())
    {
        return Fail("batch: 姿势 / 资产信息与资产的骨骼数 / 绘制项数不一致");
    }
    // 回滚点：任何一步失败都把合包恢复成追加前的样子
    const size_t n_bones = batch.bones.size();
    const size_t n_draws = batch.draws.size();
    const size_t n_insts = batch.insts.size();
    const size_t n_deform = batch.deform.size();
    const size_t n_cv = batch.cpu_verts.size();
    const size_t n_ci = batch.cpu_index.size();
    const auto fail = [&](std::string why) {
        batch.bones.resize(n_bones);
        batch.draws.resize(n_draws);
        batch.cmds.resize(n_draws);
        batch.insts.resize(n_insts);
        batch.deform.resize(n_deform);
        batch.cpu_verts.resize(n_cv);
        batch.cpu_index.resize(n_ci);
        range = SpineGpuBatchRange {};
        return Fail(std::move(why));
    };
    if (n_bones > 0xFFFFFFFFull - pose.bones.size() || n_insts >= 0xFFFFFFFFull)
    {
        return fail("batch: 合包的骨骼 / 实例数超出 uint32");
    }
    const uint32_t bone_base = static_cast<uint32_t>(n_bones);
    const uint32_t inst_index = static_cast<uint32_t>(n_insts);
    batch.bones.insert(batch.bones.end(), pose.bones.begin(), pose.bones.end());
    batch.insts.push_back(inst);

    // ---- 本实例生效的 mesh deform（S6）：接在合包已有的 deform 之后 ----
    batch.scratch_region.assign(a.draw_items.size(), -1);
    for (const SpineGpuMeshDeform& md : pose.mesh_deforms)
    {
        if (md.draw_item >= a.draw_items.size() || md.values.size() != info.deform_len[md.draw_item])
        {
            return fail("batch: 绘制项 " + std::to_string(md.draw_item) + " 的 deform 有 " +
                        std::to_string(md.values.size()) + " 个 float，应为 " +
                        (md.draw_item < a.draw_items.size() ? std::to_string(info.deform_len[md.draw_item])
                                                            : std::string("?")));
        }
        batch.scratch_region[md.draw_item] = static_cast<int64_t>(batch.deform.size());
        batch.deform.insert(batch.deform.end(), md.values.begin(), md.values.end());
    }
    range.deform_floats = static_cast<uint32_t>(batch.deform.size() - n_deform);
    if (range.deform_floats > info.deform_capacity)
    {
        // 上界是"所有 kFallback 项同时生效"；超出说明有非 fallback 的项被 deform 了 —— 容器的 kind 与
        // 解算器对不上，按静态画就是错形变，所以宁可这个实例这一帧不画、把原因报出来。
        return fail("batch: deform 需要 " + std::to_string(range.deform_floats) + " 个 float，单实例上界 " +
                    std::to_string(info.deform_capacity));
    }

    // ---- 本实例的裁剪输出：顶点 / 索引接在合包已有的之后 ----
    const uint32_t cv_base = static_cast<uint32_t>(n_cv);
    const uint32_t ci_base = static_cast<uint32_t>(n_ci);
    const bool has_clip = clip != nullptr && !clip->slot_clipped.empty();
    if (has_clip)
    {
        const size_t nv = clip->xy.size() / 2u;
        if (clip->uv.size() != clip->xy.size())
        {
            return fail("batch: 裁剪输出的 xy / uv 长度不一致");
        }
        batch.cpu_verts.reserve(n_cv + nv);
        for (size_t v = 0; v < nv; ++v)
        {
            batch.cpu_verts.push_back({clip->xy[v * 2u], clip->xy[v * 2u + 1u], clip->uv[v * 2u], clip->uv[v * 2u + 1u]});
        }
        batch.cpu_index.insert(batch.cpu_index.end(), clip->indices.begin(), clip->indices.end());
        range.clip_vertices = static_cast<uint32_t>(nv);
    }

    // ---- 可见绘制项：按绘制顺序、取槽位当前附件对应的那一项（解算器给的 slot_draw_item）----
    range.draw_first = static_cast<uint32_t>(n_draws);
    for (uint32_t s : pose.draw_order)
    {
        if (s >= pose.slot_draw_item.size() || pose.slot_draw_item[s] == SpineGpuPoseResult::kNoDrawItem)
        {
            continue;
        }
        const uint32_t item = pose.slot_draw_item[s];
        if (item >= a.draw_items.size() || s >= pose.slots.size())
        {
            return fail("batch: 槽位 " + std::to_string(s) + " 的绘制项 / 槽位下标越界");
        }
        const SpineGpuSlotSample& slot = pose.slots[s];
        const SpineDrawItem& di = a.draw_items[item];
        const bool additive = di.blend_mode == SpineBlendMode::kAdditive;
        const uint32_t ck =
            has_clip && s < clip->slot_clipped.size() ? clip->slot_clipped[s] : SpineGpuClipFrameResult::kNotClipped;
        if (ck != SpineGpuClipFrameResult::kNotClipped)
        {
            if (ck >= clip->draws.size())
            {
                return fail("batch: 槽位 " + std::to_string(s) + " 的裁剪结果下标越界");
            }
            const SpineGpuClippedDraw& cd = clip->draws[ck];
            if (cd.index_count == 0)
            {
                continue;   // 整个被裁掉
            }
            SpineGpuGpuDraw g {};
            g.vertex_format = static_cast<uint32_t>(di.vertex_format);
            g.bone_base = bone_base;
            g.inst_index = inst_index;
            g.tint = SpineGpuPackRgba8(slot.color);
            g.flags = SpineGpuDrawFlags::kStraightAlpha | SpineGpuDrawFlags::kCpuVerts |
                      (additive ? SpineGpuDrawFlags::kAdditive : 0u);
            g.deform_base = cv_base + cd.vertex_first;
            ApplyTintBlack(g, slot, tint_black_on, additive, range.tint_black);
            batch.draws.push_back(g);
            batch.cmds.push_back({item, true, ci_base + cd.index_first, cd.index_count});
            ++range.clipped;
            continue;
        }
        SpineGpuGpuDraw g {};
        g.vertex_format = static_cast<uint32_t>(di.vertex_format);
        g.uv_offset = di.uv_offset;
        g.slot_bone = di.slot_index < a.slot_bone.size() ? a.slot_bone[di.slot_index] : 0u;
        g.bone_base = bone_base;
        g.inst_index = inst_index;
        g.tint = SpineGpuPackRgba8(slot.color);
        g.flags = SpineGpuDrawFlags::kStraightAlpha | (additive ? SpineGpuDrawFlags::kAdditive : 0u);
        g.weighted_base = a.weighted_segment_offset / 4u;
        ApplyTintBlack(g, slot, tint_black_on, additive, range.tint_black);
        if (batch.scratch_region[item] >= 0)
        {
            // uint 回绕偏置：shader 用段内绝对下标（rigid）/ 全局影响下标（weighted）直接加
            const uint32_t region = static_cast<uint32_t>(batch.scratch_region[item]);
            g.deform_base = di.vertex_format == SpineVertexFormat::kRigid
                                ? region - 2u * static_cast<uint32_t>(di.vertex_base)
                                : region - 2u * info.infl_base[item];
            g.flags |= SpineGpuDrawFlags::kDeform;
            ++range.deformed;
        }
        batch.draws.push_back(g);
        batch.cmds.push_back({item, false, di.index_offset, di.index_count});
    }
    range.draw_count = static_cast<uint32_t>(batch.draws.size()) - range.draw_first;
    return Ok();
}

bool SpineGpuEmulateVertex(const SpineGpuAsset& a, const SpineGpuBatch& batch, uint32_t draw, uint32_t vi,
                           SpineGpuEmulatedVertex& out)
{
    if (draw >= batch.draws.size())
    {
        return false;
    }
    const SpineGpuGpuDraw& d = batch.draws[draw];
    const size_t n_words = a.vertex_buffer.size() / 4u;
    const auto bone = [&](uint32_t b, float m[6]) {
        // bones[] 是 6 float / 骨（a b c d wx wy）
        if (b >= batch.bones.size())
        {
            return false;
        }
        const SpineGpuBoneSample& s = batch.bones[b];
        m[0] = s.a;
        m[1] = s.b;
        m[2] = s.c;
        m[3] = s.d;
        m[4] = s.world_x;
        m[5] = s.world_y;
        return true;
    };
    const auto deform_at = [&](uint32_t f, float& x, float& y) {
        if (static_cast<size_t>(f) + 1u >= batch.deform.size())
        {
            return false;
        }
        x = batch.deform[f];
        y = batch.deform[f + 1u];
        return true;
    };
    const bool has_deform = (d.flags & SpineGpuDrawFlags::kDeform) != 0u;
    float px = 0.0f;
    float py = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
    if ((d.flags & SpineGpuDrawFlags::kCpuVerts) != 0u)
    {
        const uint32_t k = d.deform_base + vi;
        if (k >= batch.cpu_verts.size())
        {
            return false;
        }
        const SpineGpuCpuVert& cv = batch.cpu_verts[k];
        px = cv.x;
        py = cv.y;
        u = cv.u;
        v = cv.v;
    }
    else
    {
        if (d.vertex_format == 0u)
        {
            const size_t w = static_cast<size_t>(vi) * 2u;
            if (w + 1u >= n_words)
            {
                return false;
            }
            float bx = 0.0f;
            float by = 0.0f;
            std::memcpy(&bx, a.vertex_buffer.data() + w * 4u, 4);
            std::memcpy(&by, a.vertex_buffer.data() + (w + 1u) * 4u, 4);
            if (has_deform && !deform_at(d.deform_base + vi * 2u, bx, by))
            {
                return false;
            }
            float m[6];
            if (!bone(d.bone_base + d.slot_bone, m))
            {
                return false;
            }
            px = bx * m[0] + by * m[1] + m[4];
            py = bx * m[2] + by * m[3] + m[5];
        }
        else
        {
            const size_t w0 = static_cast<size_t>(d.weighted_base) + static_cast<size_t>(vi) * 2u;
            if (w0 + 1u >= n_words)
            {
                return false;
            }
            const uint32_t off = WordAt(a, w0);
            const uint32_t cnt = WordAt(a, w0 + 1u);
            float x = 0.0f;
            float y = 0.0f;
            for (uint32_t k = 0; k < cnt; ++k)
            {
                const size_t ik = static_cast<size_t>(off) + k;
                if (ik >= a.influence_bone.size() || ik * 2u + 1u >= a.influence_bind.size())
                {
                    return false;
                }
                float bx = a.influence_bind[ik * 2u];
                float by = a.influence_bind[ik * 2u + 1u];
                if (has_deform)
                {
                    float dx = 0.0f;
                    float dy = 0.0f;
                    if (!deform_at(d.deform_base + static_cast<uint32_t>(ik) * 2u, dx, dy))
                    {
                        return false;
                    }
                    bx += dx;
                    by += dy;
                }
                uint32_t bw = 0;
                std::memcpy(&bw, &a.influence_bone[ik], 4);
                float m[6];
                if (!bone(d.bone_base + (bw & 0xFFFFu), m))
                {
                    return false;
                }
                const float wt = static_cast<float>((bw >> 16u) & 0xFFu) / 255.0f;
                const float tx = bx * m[0] + by * m[1] + m[4];
                const float ty = bx * m[2] + by * m[3] + m[5];
                x += tx * wt;
                y += ty * wt;
            }
            px = x;
            py = y;
        }
        const size_t uvi = static_cast<size_t>(static_cast<uint32_t>(d.uv_offset + vi)) * 2u;
        if (uvi + 1u >= a.uv_buffer.size())
        {
            return false;
        }
        u = a.uv_buffer[uvi];
        v = a.uv_buffer[uvi + 1u];
    }
    if (d.inst_index >= batch.insts.size())
    {
        return false;
    }
    const SpineGpuGpuInst& inst = batch.insts[d.inst_index];
    const float rx = inst.root_abcd[0] * px + inst.root_abcd[1] * py + inst.root_t[0];
    const float ry = inst.root_abcd[2] * px + inst.root_abcd[3] * py + inst.root_t[1];
    const float* m = inst.clip_from_world;
    out.clip_x = m[0] * rx + m[4] * ry + m[12];
    out.clip_y = m[1] * rx + m[5] * ry + m[13];
    out.u = u;
    out.v = v;
    return true;
}

SpineGpuRect SpineGpuGridCell(uint32_t count, uint32_t index, float vw, float vh)
{
    SpineGpuRect r;
    if (count <= 1u || vw <= 0.0f || vh <= 0.0f)
    {
        r.w = vw;
        r.h = vh;
        return r;
    }
    const float aspect = vw / vh;
    uint32_t cols = static_cast<uint32_t>(std::lround(std::sqrt(static_cast<float>(count) * aspect)));
    cols = std::clamp<uint32_t>(cols, 1u, count);
    const uint32_t rows = (count + cols - 1u) / cols;
    r.w = vw / static_cast<float>(cols);
    r.h = vh / static_cast<float>(rows);
    r.x = r.w * static_cast<float>(index % cols);
    r.y = r.h * static_cast<float>(index / cols);
    return r;
}

SpineGpuGpuInst SpineGpuFitInstance(const float bounds[4], const SpineGpuRect& cell, float vw, float vh, float y_sign)
{
    // 骨架空间（y 向上，1 单位 = fit 像素）→ 视口：包围盒中心对准格子中心
    const float bw = std::max(bounds[2] - bounds[0], 1.0f);
    const float bh = std::max(bounds[3] - bounds[1], 1.0f);
    const float fit = 0.8f * std::min(cell.w / bw, cell.h / bh);   // 像素 / 单位
    const float sx = 2.0f * fit / vw;
    const float sy = y_sign * 2.0f * fit / vh;
    const float cx = 0.5f * (bounds[0] + bounds[2]);
    const float cy = 0.5f * (bounds[1] + bounds[3]);
    // 格子中心相对视口中心的像素偏移（y 向上）；单实例 = 整个视口 ⇒ 偏移为 0，与多实例之前逐位相同
    const float ox = cell.x + 0.5f * cell.w - 0.5f * vw;
    const float oy = 0.5f * vh - (cell.y + 0.5f * cell.h);
    float tx = -cx * sx;
    float ty = -cy * sy;
    if (ox != 0.0f)
    {
        tx += 2.0f * ox / vw;
    }
    if (oy != 0.0f)
    {
        ty += y_sign * 2.0f * oy / vh;
    }
    return Affine(sx, sy, tx, ty);
}

SpineGpuGpuInst SpineGpuPlaceInstance(float x, float y, float scale, float vw, float vh, float y_sign)
{
    const float sx = 2.0f * scale / vw;
    const float sy = y_sign * 2.0f * scale / vh;
    return Affine(sx, sy, 2.0f * x / vw, y_sign * 2.0f * y / vh);
}
