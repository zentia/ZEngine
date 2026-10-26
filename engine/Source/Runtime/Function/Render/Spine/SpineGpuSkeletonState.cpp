#include "Runtime/Function/Render/Spine/SpineGpuSkeletonState.h"

#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

// =====================================================================
// 参考实现：G:/OSGame/unity-engine-source/Modules/Spine/spine-cpp（3.8）
//   * RotateTimeline::apply()                 RotateTimeline.cpp:49-106
//   * TranslateTimeline::apply()              TranslateTimeline.cpp:57-109
//   * ScaleTimeline::apply()                  ScaleTimeline.cpp:55-142
//   * ShearTimeline::apply()                  ShearTimeline.cpp:52-104
//   * AttachmentTimeline::apply()             AttachmentTimeline.cpp:53-80
//   * ColorTimeline::apply()                  ColorTimeline.cpp:64-113
//   * TwoColorTimeline::apply()               TwoColorTimeline.cpp:68-149
//   * DeformTimeline::apply()                 DeformTimeline.cpp:62-258
//   * DrawOrderTimeline::apply()              DrawOrderTimeline.cpp:53-97
//   * PathConstraintPositionTimeline::apply() PathConstraintPositionTimeline.cpp:59-100
//   * PathConstraintMixTimeline::apply()      PathConstraintMixTimeline.cpp:59-116
//   * AnimationState::applyRotateTimeline()   AnimationState.cpp:636-715
//   * Slot::setToSetupPose / setAttachment    Slot.cpp:54-102
//   * Skeleton::setBones/SlotsToSetupPose     Skeleton.cpp:223-270
//   * Color::set / add（夹 [0,1]）            Color.h
// =====================================================================

namespace
{

SpineImportResult Fail(std::string message)
{
    SpineImportResult r;
    r.ok = false;
    r.error = std::move(message);
    return r;
}

SpineImportResult Ok()
{
    SpineImportResult r;
    r.ok = true;
    return r;
}

uint32_t GetU32At(const std::vector<uint8_t>& bytes, size_t pos)
{
    return static_cast<uint32_t>(bytes[pos]) | (static_cast<uint32_t>(bytes[pos + 1]) << 8) |
           (static_cast<uint32_t>(bytes[pos + 2]) << 16) | (static_cast<uint32_t>(bytes[pos + 3]) << 24);
}

// `MathUtil::clamp(x, 0, 1)`（MathUtil.cpp:57-59）—— NaN 原样通过（两个比较都为假）。
inline float Clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

// `Color::set(r, g, b, a)`（Color.h）：写完**夹到 [0,1]**。
inline void ColorSet(float* c, float r, float g, float b, float a)
{
    c[0] = Clamp01(r);
    c[1] = Clamp01(g);
    c[2] = Clamp01(b);
    c[3] = Clamp01(a);
}

// `Color::add(r, g, b, a)`（Color.h）：加完**夹到 [0,1]**。
inline void ColorAdd(float* c, float r, float g, float b, float a)
{
    c[0] = Clamp01(c[0] + r);
    c[1] = Clamp01(c[1] + g);
    c[2] = Clamp01(c[2] + b);
    c[3] = Clamp01(c[3] + a);
}

// `MathUtil::sign`（MathUtil.cpp:53-55）：0 返回 0（不是 +1）。
inline float Sign(float v)
{
    return v < 0.0f ? -1.0f : (v > 0.0f ? 1.0f : 0.0f);
}

inline float Abs(float v)
{
    return v < 0.0f ? -v : v;
}

std::string TlName(uint32_t tl_index)
{
    return "第 " + std::to_string(tl_index) + " 条时间轴";
}

// 一条时间轴的帧数组（已校验区间）。
struct FrameView
{
    const float* frames = nullptr;
    uint32_t frame_count = 0;
    uint32_t entries = 0;

    float FirstTime() const { return frames[0]; }
    float LastTime() const { return frames[static_cast<size_t>(frame_count - 1u) * entries]; }
    // 末帧第 k 个值（k 从 1 起；= 参考实现的 `_frames[_frames.size() + PREV_*]`）
    float Last(uint32_t k) const { return frames[static_cast<size_t>(frame_count - 1u) * entries + k]; }
};

SpineImportResult GetFrames(const SpineGpuAsset& asset, uint32_t tl_index, FrameView& out)
{
    if (tl_index >= asset.timelines.size())
    {
        return Fail("apply: 时间轴下标 " + std::to_string(tl_index) + " 越界");
    }
    const SpineGpuTimeline& tl = asset.timelines[tl_index];
    const uint32_t entries = SpineTimelineEntries(static_cast<SpineTimelineType>(tl.type));
    if (entries == 0 || tl.frame_count == 0)
    {
        return Fail("apply: " + TlName(tl_index) + "类型/帧数不自洽");
    }
    const uint64_t end = static_cast<uint64_t>(tl.frame_first) + static_cast<uint64_t>(tl.frame_count) * entries;
    if (end > asset.frames.size())
    {
        return Fail("apply: " + TlName(tl_index) + "的帧区间越界");
    }
    out.frames = asset.frames.data() + tl.frame_first;
    out.frame_count = tl.frame_count;
    out.entries = entries;
    return Ok();
}

// 两个值的曲线时间轴（translate / scale / shear / pathMix）在 `time` 处的原始插值结果
// （还没乘 setup、没混合）。调用方保证 `time >= frames[0]`。
SpineImportResult SampleTwo(const SpineGpuAsset& asset, uint32_t tl_index, const FrameView& fv, float time, float& x,
                            float& y)
{
    if (time >= fv.LastTime())
    {
        x = fv.Last(1);
        y = fv.Last(2);
        return Ok();
    }
    const uint32_t cur = SpineGpuBinarySearchFrames(fv.frames, fv.frame_count, fv.entries, time);
    float percent = 0.0f;
    const SpineImportResult cr =
        SpineGpuCurvePercentAt(asset, asset.timelines[tl_index], tl_index, cur, time, percent);
    if (!cr.ok)
    {
        return cr;
    }
    const size_t prev = static_cast<size_t>(cur) - fv.entries;
    x = fv.frames[prev + 1u];
    y = fv.frames[prev + 2u];
    // `x += (_frames[frame + X] - x) * percent`（TranslateTimeline.cpp:101-102）
    x += (fv.frames[cur + 1u] - x) * percent;
    y += (fv.frames[cur + 2u] - y) * percent;
    return Ok();
}

// ---------------------------------------------------------------------
// 骨骼时间轴
// ---------------------------------------------------------------------

// RotateTimeline.cpp:49-106
SpineImportResult ApplyRotate(const SpineGpuAsset& asset, SpineGpuSkeletonState& st, uint32_t tl_index,
                              const FrameView& fv, float time, float alpha, SpineGpuMixBlend blend)
{
    const SpineGpuTimeline& tl = asset.timelines[tl_index];
    if (tl.target >= st.bones.size())
    {
        return Fail("apply: " + TlName(tl_index) + "（rotate）的骨骼下标越界");
    }
    SpineGpuBoneState& bone = st.bones[tl.target];
    const float setup = asset.bones[tl.target].rotation;

    if (time < fv.FirstTime())
    {
        switch (blend)
        {
            case SpineGpuMixBlend::kSetup:
                bone.rotation = setup;
                break;
            case SpineGpuMixBlend::kFirst:
            {
                const float r = setup - bone.rotation;
                bone.rotation += SpineGpuWrapDegrees(r) * alpha;
                break;
            }
            default:
                break;
        }
        return Ok();
    }

    if (time >= fv.LastTime())
    {
        float r = fv.Last(1);
        switch (blend)
        {
            case SpineGpuMixBlend::kSetup:
                bone.rotation = setup + r * alpha;
                break;
            case SpineGpuMixBlend::kFirst:
            case SpineGpuMixBlend::kReplace:
                r += setup - bone.rotation;
                r = SpineGpuWrapDegrees(r);
                [[fallthrough]];
            case SpineGpuMixBlend::kAdd:
                bone.rotation += r * alpha;
                break;
        }
        return Ok();
    }

    const uint32_t cur = SpineGpuBinarySearchFrames(fv.frames, fv.frame_count, fv.entries, time);
    const float prev_rotation = fv.frames[static_cast<size_t>(cur) - 1u];   // frame + PREV_ROTATION(-1)
    float percent = 0.0f;
    const SpineImportResult cr = SpineGpuCurvePercentAt(asset, tl, tl_index, cur, time, percent);
    if (!cr.ok)
    {
        return cr;
    }
    float r = fv.frames[cur + 1u] - prev_rotation;
    r = prev_rotation + SpineGpuWrapDegrees(r) * percent;
    switch (blend)
    {
        case SpineGpuMixBlend::kSetup:
            bone.rotation = setup + SpineGpuWrapDegrees(r) * alpha;
            break;
        case SpineGpuMixBlend::kFirst:
        case SpineGpuMixBlend::kReplace:
            r += setup - bone.rotation;
            [[fallthrough]];
        case SpineGpuMixBlend::kAdd:
            bone.rotation += SpineGpuWrapDegrees(r) * alpha;
            break;
    }
    return Ok();
}

// TranslateTimeline.cpp:57-109 与 ShearTimeline.cpp:52-104：两者逐行同构，只是写的字段不同。
SpineImportResult ApplyTranslateOrShear(const SpineGpuAsset& asset, SpineGpuSkeletonState& st, uint32_t tl_index,
                                        const FrameView& fv, float time, float alpha, SpineGpuMixBlend blend,
                                        bool shear)
{
    const SpineGpuTimeline& tl = asset.timelines[tl_index];
    if (tl.target >= st.bones.size())
    {
        return Fail("apply: " + TlName(tl_index) + "的骨骼下标越界");
    }
    SpineGpuBoneState& bone = st.bones[tl.target];
    const SpineGpuBone& data = asset.bones[tl.target];
    float& bx = shear ? bone.shear_x : bone.x;
    float& by = shear ? bone.shear_y : bone.y;
    const float dx = shear ? data.shear_x : data.x;
    const float dy = shear ? data.shear_y : data.y;

    if (time < fv.FirstTime())
    {
        switch (blend)
        {
            case SpineGpuMixBlend::kSetup:
                bx = dx;
                by = dy;
                return Ok();
            case SpineGpuMixBlend::kFirst:
                bx += (dx - bx) * alpha;
                by += (dy - by) * alpha;
                break;
            default:
                break;
        }
        return Ok();
    }

    float x = 0.0f;
    float y = 0.0f;
    const SpineImportResult sr = SampleTwo(asset, tl_index, fv, time, x, y);
    if (!sr.ok)
    {
        return sr;
    }
    switch (blend)
    {
        case SpineGpuMixBlend::kSetup:
            bx = dx + x * alpha;
            by = dy + y * alpha;
            break;
        case SpineGpuMixBlend::kFirst:
        case SpineGpuMixBlend::kReplace:
            bx += (dx + x - bx) * alpha;
            by += (dy + y - by) * alpha;
            break;
        case SpineGpuMixBlend::kAdd:
            bx += x * alpha;
            by += y * alpha;
            break;
    }
    return Ok();
}

// ScaleTimeline.cpp:55-142
//
// ⚠️ 帧里存的是**倍率**，取出来先乘 setup（:69-70 / :80-81）；alpha < 1 时按方向分两套
//    "符号取谁"（混出取 setup / 当前的符号，混入取关键帧的符号）—— 写反了只在负缩放
//    （镜像）骨骼上露出来，表现是混合途中突然翻面。
SpineImportResult ApplyScale(const SpineGpuAsset& asset, SpineGpuSkeletonState& st, uint32_t tl_index,
                             const FrameView& fv, float time, float alpha, SpineGpuMixBlend blend,
                             SpineGpuMixDirection direction)
{
    const SpineGpuTimeline& tl = asset.timelines[tl_index];
    if (tl.target >= st.bones.size())
    {
        return Fail("apply: " + TlName(tl_index) + "（scale）的骨骼下标越界");
    }
    SpineGpuBoneState& bone = st.bones[tl.target];
    const SpineGpuBone& data = asset.bones[tl.target];

    if (time < fv.FirstTime())
    {
        switch (blend)
        {
            case SpineGpuMixBlend::kSetup:
                bone.scale_x = data.scale_x;
                bone.scale_y = data.scale_y;
                return Ok();
            case SpineGpuMixBlend::kFirst:
                bone.scale_x += (data.scale_x - bone.scale_x) * alpha;
                bone.scale_y += (data.scale_y - bone.scale_y) * alpha;
                break;
            default:
                break;
        }
        return Ok();
    }

    float x = 0.0f;
    float y = 0.0f;
    if (time >= fv.LastTime())
    {
        x = fv.Last(1) * data.scale_x;
        y = fv.Last(2) * data.scale_y;
    }
    else
    {
        const uint32_t cur = SpineGpuBinarySearchFrames(fv.frames, fv.frame_count, fv.entries, time);
        float percent = 0.0f;
        const SpineImportResult cr = SpineGpuCurvePercentAt(asset, tl, tl_index, cur, time, percent);
        if (!cr.ok)
        {
            return cr;
        }
        const size_t prev = static_cast<size_t>(cur) - fv.entries;
        x = fv.frames[prev + 1u];
        y = fv.frames[prev + 2u];
        x = (x + (fv.frames[cur + 1u] - x) * percent) * data.scale_x;
        y = (y + (fv.frames[cur + 2u] - y) * percent) * data.scale_y;
    }

    if (alpha == 1.0f)
    {
        if (blend == SpineGpuMixBlend::kAdd)
        {
            bone.scale_x += x - data.scale_x;
            bone.scale_y += y - data.scale_y;
        }
        else
        {
            bone.scale_x = x;
            bone.scale_y = y;
        }
        return Ok();
    }

    float bx;
    float by;
    if (direction == SpineGpuMixDirection::kOut)
    {
        switch (blend)
        {
            case SpineGpuMixBlend::kSetup:
                bx = data.scale_x;
                by = data.scale_y;
                bone.scale_x = bx + (Abs(x) * Sign(bx) - bx) * alpha;
                bone.scale_y = by + (Abs(y) * Sign(by) - by) * alpha;
                break;
            case SpineGpuMixBlend::kFirst:
            case SpineGpuMixBlend::kReplace:
                bx = bone.scale_x;
                by = bone.scale_y;
                bone.scale_x = bx + (Abs(x) * Sign(bx) - bx) * alpha;
                bone.scale_y = by + (Abs(y) * Sign(by) - by) * alpha;
                break;
            case SpineGpuMixBlend::kAdd:
                bx = bone.scale_x;
                by = bone.scale_y;
                bone.scale_x = bx + (Abs(x) * Sign(bx) - data.scale_x) * alpha;
                bone.scale_y = by + (Abs(y) * Sign(by) - data.scale_y) * alpha;
                break;
        }
    }
    else
    {
        switch (blend)
        {
            case SpineGpuMixBlend::kSetup:
                bx = Abs(data.scale_x) * Sign(x);
                by = Abs(data.scale_y) * Sign(y);
                bone.scale_x = bx + (x - bx) * alpha;
                bone.scale_y = by + (y - by) * alpha;
                break;
            case SpineGpuMixBlend::kFirst:
            case SpineGpuMixBlend::kReplace:
                bx = Abs(bone.scale_x) * Sign(x);
                by = Abs(bone.scale_y) * Sign(y);
                bone.scale_x = bx + (x - bx) * alpha;
                bone.scale_y = by + (y - by) * alpha;
                break;
            case SpineGpuMixBlend::kAdd:
                bx = Sign(x);
                by = Sign(y);
                bone.scale_x = Abs(bone.scale_x) * bx + (x - Abs(data.scale_x) * bx) * alpha;
                bone.scale_y = Abs(bone.scale_y) * by + (y - Abs(data.scale_y) * by) * alpha;
                break;
        }
    }
    return Ok();
}

// ---------------------------------------------------------------------
// 槽位时间轴
// ---------------------------------------------------------------------

// ColorTimeline.cpp:64-113 / TwoColorTimeline.cpp:68-149
SpineImportResult ApplyColor(const SpineGpuAsset& asset, SpineGpuSkeletonState& st, uint32_t tl_index,
                             const FrameView& fv, float time, float alpha, SpineGpuMixBlend blend, bool two)
{
    const SpineGpuTimeline& tl = asset.timelines[tl_index];
    if (tl.target >= st.slots.size())
    {
        return Fail("apply: " + TlName(tl_index) + "（颜色）的槽位下标越界");
    }
    SpineGpuSlotState& slot = st.slots[tl.target];
    const SpineGpuSlot& data = asset.slots[tl.target];
    float* color = slot.color;
    float* dark = slot.dark;

    if (time < fv.FirstTime())
    {
        switch (blend)
        {
            case SpineGpuMixBlend::kSetup:
                ColorSet(color, data.color_r, data.color_g, data.color_b, data.color_a);
                if (two)
                {
                    ColorSet(dark, data.dark_r, data.dark_g, data.dark_b, data.dark_a);
                }
                return Ok();
            case SpineGpuMixBlend::kFirst:
                if (two)
                {
                    // ⚠️ 参考实现的笔误，**照抄**（TwoColorTimeline.cpp:84-93）：
                    //    写的是 `color += (color - setup) * alpha`（ColorTimeline 写的是 setup - color），
                    //    而且直接 `+=`、**不夹** [0,1]。
                    color[0] += (color[0] - data.color_r) * alpha;
                    color[1] += (color[1] - data.color_g) * alpha;
                    color[2] += (color[2] - data.color_b) * alpha;
                    color[3] += (color[3] - data.color_a) * alpha;
                    dark[0] += (dark[0] - data.dark_r) * alpha;
                    dark[1] += (dark[1] - data.dark_g) * alpha;
                    dark[2] += (dark[2] - data.dark_b) * alpha;
                }
                else
                {
                    ColorAdd(color, (data.color_r - color[0]) * alpha, (data.color_g - color[1]) * alpha,
                             (data.color_b - color[2]) * alpha, (data.color_a - color[3]) * alpha);
                }
                return Ok();
            default:
                return Ok();
        }
    }

    float v[7] = {0, 0, 0, 0, 0, 0, 0};
    const uint32_t nv = two ? 7u : 4u;
    if (time >= fv.LastTime())
    {
        for (uint32_t k = 0; k < nv; ++k)
        {
            v[k] = fv.Last(k + 1u);
        }
    }
    else
    {
        const uint32_t cur = SpineGpuBinarySearchFrames(fv.frames, fv.frame_count, fv.entries, time);
        float percent = 0.0f;
        const SpineImportResult cr = SpineGpuCurvePercentAt(asset, tl, tl_index, cur, time, percent);
        if (!cr.ok)
        {
            return cr;
        }
        const size_t prev = static_cast<size_t>(cur) - fv.entries;
        for (uint32_t k = 0; k < nv; ++k)
        {
            v[k] = fv.frames[prev + 1u + k];
            v[k] += (fv.frames[cur + 1u + k] - v[k]) * percent;
        }
    }

    if (alpha == 1.0f)
    {
        ColorSet(color, v[0], v[1], v[2], v[3]);
        if (two)
        {
            // ⚠️ 暗色 alpha **写死 1**（TwoColorTimeline.cpp:139）
            ColorSet(dark, v[4], v[5], v[6], 1.0f);
        }
        return Ok();
    }
    if (blend == SpineGpuMixBlend::kSetup)
    {
        ColorSet(color, data.color_r, data.color_g, data.color_b, data.color_a);
        if (two)
        {
            ColorSet(dark, data.dark_r, data.dark_g, data.dark_b, data.dark_a);
        }
    }
    ColorAdd(color, (v[0] - color[0]) * alpha, (v[1] - color[1]) * alpha, (v[2] - color[2]) * alpha,
             (v[3] - color[3]) * alpha);
    if (two)
    {
        ColorAdd(dark, (v[4] - dark[0]) * alpha, (v[5] - dark[1]) * alpha, (v[6] - dark[2]) * alpha, 0.0f);
    }
    return Ok();
}

// AttachmentTimeline.cpp:53-80 —— **alpha 不参与**（附件不能"混一半"）。
SpineImportResult ApplyAttachment(const SpineGpuAsset& asset, const SpineGpuRuntimeData& data,
                                  SpineGpuSkeletonState& st, uint32_t tl_index, const FrameView& fv, float time,
                                  SpineGpuMixBlend blend, SpineGpuMixDirection direction)
{
    const SpineGpuTimeline& tl = asset.timelines[tl_index];
    if (tl.target >= st.slots.size() || tl.target >= data.slot_setup.size())
    {
        return Fail("apply: " + TlName(tl_index) + "（attachment）的槽位下标越界");
    }
    SpineGpuSlotState& slot = st.slots[tl.target];
    // `skeleton.getAttachment(slotIndex, name)`：按骨架**当前**皮肤解析
    const auto attach = [&](const SpineGpuAttachRef& ref) {
        SpineGpuSetSlotAttachment(slot, SpineGpuResolveAttachment(data, st.skin, tl.target, ref));
    };
    if (direction == SpineGpuMixDirection::kOut && blend == SpineGpuMixBlend::kSetup)
    {
        attach(data.slot_setup[tl.target]);
        return Ok();
    }
    if (time < fv.FirstTime())
    {
        if (blend == SpineGpuMixBlend::kSetup || blend == SpineGpuMixBlend::kFirst)
        {
            attach(data.slot_setup[tl.target]);
        }
        return Ok();
    }
    uint32_t frame_index = fv.frame_count - 1u;
    if (time < fv.LastTime())
    {
        frame_index = SpineGpuBinarySearchFrames(fv.frames, fv.frame_count, 1u, time) - 1u;
    }
    const size_t ri = static_cast<size_t>(data.timeline_ref_first[tl_index]) + frame_index;
    if (ri >= data.attach_refs.size())
    {
        return Fail("apply: " + TlName(tl_index) + "（attachment）的帧查找表越界");
    }
    attach(data.attach_refs[ri]);
    return Ok();
}

inline void ResetDrawOrder(SpineGpuSkeletonState& st)
{
    for (uint32_t i = 0; i < st.draw_order.size(); ++i)
    {
        st.draw_order[i] = i;
    }
}

// DrawOrderTimeline.cpp:53-97 —— **alpha 不参与**。
SpineImportResult ApplyDrawOrder(const SpineGpuRuntimeData& data, SpineGpuSkeletonState& st, uint32_t tl_index,
                                 const FrameView& fv, float time, SpineGpuMixBlend blend,
                                 SpineGpuMixDirection direction)
{
    if (direction == SpineGpuMixDirection::kOut && blend == SpineGpuMixBlend::kSetup)
    {
        ResetDrawOrder(st);
        return Ok();
    }
    if (time < fv.FirstTime())
    {
        if (blend == SpineGpuMixBlend::kSetup || blend == SpineGpuMixBlend::kFirst)
        {
            ResetDrawOrder(st);
        }
        return Ok();
    }
    uint32_t frame = fv.frame_count - 1u;
    if (time < fv.LastTime())
    {
        frame = SpineGpuBinarySearchFrames(fv.frames, fv.frame_count, 1u, time) - 1u;
    }
    const size_t fi = static_cast<size_t>(data.draw_order_first[tl_index]) + frame;
    if (fi >= data.draw_order_frame_pos.size())
    {
        return Fail("apply: " + TlName(tl_index) + "（drawOrder）的帧查找表越界");
    }
    const uint32_t pos = data.draw_order_frame_pos[fi];
    if (pos == SpineGpuAttachRef::kNone)
    {
        ResetDrawOrder(st);   // `drawOrderToSetupIndex.size() == 0`（:87-92）
        return Ok();
    }
    const size_t n = st.draw_order.size();
    if (static_cast<size_t>(pos) + n > data.draw_order_pool.size())
    {
        return Fail("apply: " + TlName(tl_index) + "（drawOrder）的排列越界");
    }
    for (size_t i = 0; i < n; ++i)
    {
        st.draw_order[i] = data.draw_order_pool[pos + i];
    }
    return Ok();
}

// ---------------------------------------------------------------------
// path 约束时间轴
// ---------------------------------------------------------------------

// PathConstraintPositionTimeline.cpp:59-100
SpineImportResult ApplyPathPosition(const SpineGpuAsset& asset, SpineGpuSkeletonState& st, uint32_t tl_index,
                                    const FrameView& fv, float time, float alpha, SpineGpuMixBlend blend)
{
    const SpineGpuTimeline& tl = asset.timelines[tl_index];
    if (tl.target >= st.path_position.size())
    {
        return Fail("apply: " + TlName(tl_index) + "（pathPosition）的约束下标越界");
    }
    float& pos = st.path_position[tl.target];
    const float setup = asset.paths[tl.target].position;
    if (time < fv.FirstTime())
    {
        switch (blend)
        {
            case SpineGpuMixBlend::kSetup:
                pos = setup;
                return Ok();
            case SpineGpuMixBlend::kFirst:
                pos += (setup - pos) * alpha;
                return Ok();
            default:
                return Ok();
        }
    }
    float position;
    if (time >= fv.LastTime())
    {
        position = fv.Last(1);
    }
    else
    {
        const uint32_t cur = SpineGpuBinarySearchFrames(fv.frames, fv.frame_count, fv.entries, time);
        float percent = 0.0f;
        const SpineImportResult cr = SpineGpuCurvePercentAt(asset, tl, tl_index, cur, time, percent);
        if (!cr.ok)
        {
            return cr;
        }
        position = fv.frames[static_cast<size_t>(cur) - fv.entries + 1u];
        position += (fv.frames[cur + 1u] - position) * percent;
    }
    if (blend == SpineGpuMixBlend::kSetup)
    {
        pos = setup + (position - setup) * alpha;
    }
    else
    {
        pos += (position - pos) * alpha;
    }
    return Ok();
}

// PathConstraintMixTimeline.cpp:59-116
SpineImportResult ApplyPathMix(const SpineGpuAsset& asset, SpineGpuSkeletonState& st, uint32_t tl_index,
                               const FrameView& fv, float time, float alpha, SpineGpuMixBlend blend)
{
    const SpineGpuTimeline& tl = asset.timelines[tl_index];
    if (tl.target >= st.path_rotate_mix.size())
    {
        return Fail("apply: " + TlName(tl_index) + "（pathMix）的约束下标越界");
    }
    float& rm = st.path_rotate_mix[tl.target];
    float& tm = st.path_translate_mix[tl.target];
    const float srm = asset.paths[tl.target].rotate_mix;
    const float stm = asset.paths[tl.target].translate_mix;
    if (time < fv.FirstTime())
    {
        switch (blend)
        {
            case SpineGpuMixBlend::kSetup:
                rm = srm;
                tm = stm;
                return Ok();
            case SpineGpuMixBlend::kFirst:
                rm += (srm - rm) * alpha;
                tm += (stm - tm) * alpha;
                return Ok();
            default:
                return Ok();
        }
    }
    float rotate = 0.0f;
    float translate = 0.0f;
    const SpineImportResult sr = SampleTwo(asset, tl_index, fv, time, rotate, translate);
    if (!sr.ok)
    {
        return sr;
    }
    if (blend == SpineGpuMixBlend::kSetup)
    {
        rm = srm + (rotate - srm) * alpha;
        tm = stm + (translate - stm) * alpha;
    }
    else
    {
        rm += (rotate - rm) * alpha;
        tm += (translate - tm) * alpha;
    }
    return Ok();
}

// ---------------------------------------------------------------------
// deform
// ---------------------------------------------------------------------

// 第 fr 帧在容器里的样子：`[start:u32][count:u32][count 个 float]`，只存**增量**
// （规格 §5.2，省 59.5% 载荷）⇒ `count` 之外的值恒为 0。
// ⚠️ `src` 是字节缓冲里的地址，**不保证 4 对齐**，逐元素读必须走 memcpy（x86 上就是一条
// 非对齐 load，编译器会内联掉）。
struct DeformFrameRun
{
    uint32_t start = 0;
    uint32_t count = 0;
    const uint8_t* src = nullptr;
};

bool GetDeformFrameRun(const SpineGpuAsset& asset, const SpineGpuRuntimeData& data, uint32_t tl_index, uint32_t fr,
                       uint32_t deform_len, DeformFrameRun& out)
{
    const size_t fi = static_cast<size_t>(data.deform_frame_first[tl_index]) + fr;
    if (fi >= data.deform_frame_pos.size())
    {
        return false;
    }
    const size_t pos = data.deform_frame_pos[fi];
    uint32_t start = 0;
    uint32_t count = 0;
    std::memcpy(&start, asset.timeline_payload.data() + pos, 4);
    std::memcpy(&count, asset.timeline_payload.data() + pos + 4u, 4);
    if (static_cast<uint64_t>(start) + count > deform_len)
    {
        return false;
    }
    out.start = start;
    out.count = count;
    out.src = asset.timeline_payload.data() + pos + 8u;
    return true;
}

// 第 fr 帧的"整份"数组（参考实现**读文件时**拼出来的，SkeletonBinary.cpp:986-1016）：
// 稀疏区间外补 0，rigid 再逐个加上 setup 顶点（rigid 存绝对坐标、weighted 存偏移）。
//
// ⚠️ 2026-10-23：试过"α==1 时不物化 prev/next、直接从稀疏帧 + setup 一趟写进 vertices"
//    （规格 §17.4 的 P1）—— 访存从 8.01 次/float 降到 3.18 次，但**反而慢 1.7~3.5%**：
//    逐元素要判 `i` 在不在稀疏区间里 ⇒ 循环带分支、又只能标量跑，而展开出来的三趟
//    （fill / memcpy / 加 setup）都是连续直线循环、能向量化。**这里省的是访存，亏的是 SIMD。**
//    真正有效的是给这几条热循环加 `__restrict`（见下面与 `ApplyDeform`，规格 §17.5）。
bool ExpandDeformFrame(const SpineGpuAsset& asset, const SpineGpuRuntimeData& data, uint32_t tl_index, uint32_t fr,
                       uint32_t deform_len, const float* rigid_setup, std::vector<float>& full)
{
    DeformFrameRun run;
    if (!GetDeformFrameRun(asset, data, tl_index, fr, deform_len, run))
    {
        return false;
    }
    full.assign(deform_len, 0.0f);
    if (run.count != 0)
    {
        std::memcpy(full.data() + run.start, run.src, static_cast<size_t>(run.count) * 4u);
    }
    if (rigid_setup != nullptr)
    {
        // `__restrict`：`full` 是 thread_local scratch、`rigid_setup` 指进 `asset.vertex_buffer`
        // （或 path 顶点表），两者**不可能**重叠；不写这句 MSVC 不敢向量化这段
        // （实测 3.0~4.7%，规格 §17.5）。
        float* __restrict dst = full.data();
        const float* __restrict rs = rigid_setup;
        for (uint32_t q = 0; q < deform_len; ++q)
        {
            dst[q] += rs[q];
        }
    }
    return true;
}

// `Vector::setFixedSize(n, 0)`（Vector.h:98-116）：变长补 0、变短截断、不变不动。
inline void SetFixedSize(std::vector<float>& v, size_t n)
{
    v.resize(n, 0.0f);
}

// deform 的临时数组（**跨帧复用**，与 `PoseSolver` 的 `thread_local PoseScratch` 同一招）。
//
// ⚠️ 2026-10-23 之前这里是 4 个**局部** `std::vector`（setup_buf / last / prev_v / next_v）
//    ⇒ 每条 deform 时间轴**每帧** 2~4 次堆分配（malloc + 整块清零 + free）。deform 占 anim 段
//    ~40%（按类型消融实测），而其中很大一块就是这几次分配 —— 参考实现读文件时就把每帧拼成
//    **稠密**数组了，运行时一次都不分配（DeformTimeline.cpp:182-257 直接读 `_frames`）。
//    我们不落稠密帧（规格 §5.2：只存增量，省 59.5% 的载荷），所以展开是运行时的事，
//    但"展开用的缓冲区"必须复用。
//
// ⚠️ 2026-10-23 之二：`setup` 已从本结构里去掉。它装的其实是 rigid 的 setup 顶点，而那批顶点
//    在容器里本来就是**连续**的（`kRigidVertexStride = 8` B/顶点、每个顶点就是两个 float），
//    每帧把它 memcpy 到 scratch 再读纯属白干 —— 实测 0.20 µs/帧（规格 §17.2）。现在直接把
//    `setup` 指进 `asset.vertex_buffer`（见 `RigidSetupAt`），一次指针运算就够，
//    既省了拷贝，也不用为缓存再付一份常驻内存。
struct DeformScratch
{
    std::vector<float> prev;    // 前一帧展开结果（**也**兼作"末帧"分支的 `last`）
    std::vector<float> next;    // 后一帧展开结果
};
thread_local DeformScratch t_deform_scratch;

// rigid 段里第 `byte_offset` 字节处的 setup 顶点（`byte_offset` 必须是 `kRigidVertexStride` 的整数倍）。
//
// rigid 段从 `vertex_buffer` 的第 0 字节起、`weighted_segment_offset` 处结束，段内 8 B/顶点
// （x, y 两个 float）—— 所以字节偏移天然 4 B 对齐，可以直接当 `const float*` 读。
// 与 `SpineGpuClipper` 的 `F32At(asset.vertex_buffer, at)` 逐字节读等价，只是省掉每帧的 memcpy。
// ⚠️ 调用方**必须**先做完区间校验（`byte0 + len * 4 <= weighted_segment_offset`）。
inline const float* RigidSetupAt(const SpineGpuAsset& asset, size_t byte_offset)
{
    return reinterpret_cast<const float*>(asset.vertex_buffer.data() + byte_offset);
}

// DeformTimeline.cpp:62-258
SpineImportResult ApplyDeform(const SpineGpuAsset& asset, const SpineGpuRuntimeData& data,
                              SpineGpuSkeletonState& st, uint32_t tl_index, const FrameView& fv, float time,
                              float alpha, SpineGpuMixBlend blend)
{
    const SpineGpuTimeline& tl = asset.timelines[tl_index];

    // ---- 解析"作用于谁"：槽位 + `applyDeform` 判据（:73-79）----
    uint32_t slot_index = 0;
    uint32_t deform_len = 0;
    bool rigid = false;
    const float* setup = nullptr;   // rigid 的 setup 顶点（参考实现的 `attachment->getVertices()`）
    if ((tl.target & SpineGpuConstants::kDeformTargetPathBit) != 0)
    {
        const uint32_t k = tl.target & SpineGpuConstants::kDeformTargetIndexMask;
        if (k >= asset.path_attachments.size())
        {
            return Fail("apply: " + TlName(tl_index) + "指向越界的 path 附件 " + std::to_string(k));
        }
        const SpineGpuPathAttachment& att = asset.path_attachments[k];
        slot_index = att.slot_index;
        if (slot_index >= st.slots.size())
        {
            return Fail("apply: " + TlName(tl_index) + "的槽位越界");
        }
        // 槽位此刻挂的不是这条 path（也不是任何 VertexAttachment）⇒ 整条不生效
        if (st.slots[slot_index].attachment.path_att != k)
        {
            return Ok();
        }
        deform_len = SpineGpuPathDeformLength(att);
        rigid = att.bone_count == 0;
        if (rigid)
        {
            if (static_cast<size_t>(att.vertex_offset) + deform_len > asset.path_vertices.size())
            {
                return Fail("apply: path 附件 " + std::to_string(k) + " 的顶点区间越界");
            }
            setup = asset.path_vertices.data() + att.vertex_offset;
        }
    }
    else if ((tl.target & SpineGpuConstants::kDeformTargetClipBit) != 0)
    {
        // v15：裁剪附件的 deform（改的是裁剪多边形；`VertexAttachment::applyDeform` 对非 mesh
        // 就是 `this == source`）
        const uint32_t k = tl.target & SpineGpuConstants::kDeformTargetIndexMask;
        if (k >= asset.clip_polygons.size())
        {
            return Fail("apply: " + TlName(tl_index) + "指向越界的裁剪多边形 " + std::to_string(k));
        }
        const SpineGpuClipPolygon& clip = asset.clip_polygons[k];
        slot_index = clip.slot_index;
        if (slot_index >= st.slots.size())
        {
            return Fail("apply: " + TlName(tl_index) + "的槽位越界");
        }
        if (st.slots[slot_index].attachment.clip != k)
        {
            return Ok();   // 槽位此刻挂的不是这条裁剪附件
        }
        deform_len = SpineGpuClipDeformLength(asset, clip);
        rigid = clip.vertex_format == SpineVertexFormat::kRigid;
        if (rigid)
        {
            const size_t byte0 = static_cast<size_t>(clip.vertex_offset) * SpineGpuConstants::kRigidVertexStride;
            if (byte0 + static_cast<size_t>(deform_len) * 4u > asset.weighted_segment_offset)
            {
                return Fail("apply: 裁剪多边形 " + std::to_string(k) + " 的 rigid 顶点区间越过 rigid 段");
            }
            // 不拷贝：rigid 段的顶点布局就是 `[x0, y0, x1, y1, ...]`，直接指进去读。
            setup = RigidSetupAt(asset, byte0);
        }
    }
    else
    {
        if (tl.target >= asset.attachment_records.size())
        {
            return Fail("apply: " + TlName(tl_index) + "指向越界的附件记录 " + std::to_string(tl.target));
        }
        const SpineGpuAttachmentRecord& rec = asset.attachment_records[tl.target];
        slot_index = rec.slot_index;
        if (slot_index >= st.slots.size())
        {
            return Fail("apply: " + TlName(tl_index) + "的槽位越界");
        }
        const uint32_t item = st.slots[slot_index].attachment.draw_item;
        if (item == SpineGpuAttachRef::kNone)
        {
            return Ok();   // 槽位不挂附件 / 挂的不产生绘制项
        }
        const SpineDrawItem& di = asset.draw_items[item];
        // `MeshAttachment::applyDeform()`：自己，或 inheritDeform 的直接父附件
        if (di.attachment_id != rec.attachment_id && di.deform_parent != rec.attachment_id)
        {
            return Ok();
        }
        deform_len = data.item_deform_len[item];
        rigid = di.vertex_format == SpineVertexFormat::kRigid;
        if (rigid)
        {
            const size_t byte0 = static_cast<size_t>(di.vertex_base) * SpineGpuConstants::kRigidVertexStride;
            if (byte0 + static_cast<size_t>(deform_len) * 4u > asset.weighted_segment_offset)
            {
                return Fail("apply: 绘制项 " + std::to_string(item) + " 的 rigid 顶点区间越过 rigid 段");
            }
            // 同上：rigid 段内直接指进去读，不每帧 memcpy。
            setup = RigidSetupAt(asset, byte0);
        }
    }

    std::vector<float>& vertices = st.slots[slot_index].deform;
    if (vertices.empty())
    {
        blend = SpineGpuMixBlend::kSetup;   // :81-84
    }
    const size_t n = deform_len;

    if (time < fv.FirstTime())
    {
        switch (blend)
        {
            case SpineGpuMixBlend::kSetup:
                vertices.clear();
                return Ok();
            case SpineGpuMixBlend::kFirst:
            {
                if (alpha == 1.0f)
                {
                    vertices.clear();
                    return Ok();
                }
                SetFixedSize(vertices, n);
                if (rigid)
                {
                    for (size_t i = 0; i < n; ++i)
                    {
                        vertices[i] += (setup[i] - vertices[i]) * alpha;
                    }
                }
                else
                {
                    const float a = 1.0f - alpha;
                    for (size_t i = 0; i < n; ++i)
                    {
                        vertices[i] *= a;
                    }
                }
                return Ok();
            }
            default:
                return Ok();
        }
    }

    SetFixedSize(vertices, n);

    if (time >= fv.LastTime())
    {
        std::vector<float>& last = t_deform_scratch.prev;   // 复用（这条分支不会同时用 prev）
        if (!ExpandDeformFrame(asset, data, tl_index, fv.frame_count - 1u, deform_len, rigid ? setup : nullptr, last))
        {
            return Fail("apply: " + TlName(tl_index) + "（deform）的帧超出 deform 长度");
        }
        if (alpha == 1.0f)
        {
            if (blend == SpineGpuMixBlend::kAdd)
            {
                if (rigid)
                {
                    for (size_t i = 0; i < n; ++i)
                    {
                        vertices[i] += last[i] - setup[i];
                    }
                }
                else
                {
                    for (size_t i = 0; i < n; ++i)
                    {
                        vertices[i] += last[i];
                    }
                }
            }
            else
            {
                std::memcpy(vertices.data(), last.data(), n * sizeof(float));   // :139
            }
            return Ok();
        }
        switch (blend)
        {
            case SpineGpuMixBlend::kSetup:
                if (rigid)
                {
                    for (size_t i = 0; i < n; ++i)
                    {
                        const float s = setup[i];
                        vertices[i] = s + (last[i] - s) * alpha;
                    }
                }
                else
                {
                    for (size_t i = 0; i < n; ++i)
                    {
                        vertices[i] = last[i] * alpha;
                    }
                }
                break;
            case SpineGpuMixBlend::kFirst:
            case SpineGpuMixBlend::kReplace:
                for (size_t i = 0; i < n; ++i)
                {
                    vertices[i] += (last[i] - vertices[i]) * alpha;
                }
                break;
            case SpineGpuMixBlend::kAdd:
                if (rigid)
                {
                    for (size_t i = 0; i < n; ++i)
                    {
                        vertices[i] += (last[i] - setup[i]) * alpha;
                    }
                }
                else
                {
                    for (size_t i = 0; i < n; ++i)
                    {
                        vertices[i] += last[i] * alpha;
                    }
                }
                break;
        }
        return Ok();
    }

    // 插值（:182-257）
    const uint32_t cur = SpineGpuBinarySearchFrames(fv.frames, fv.frame_count, 1u, time);
    float percent = 0.0f;
    const SpineImportResult cr = SpineGpuCurvePercentAt(asset, tl, tl_index, cur, time, percent);
    if (!cr.ok)
    {
        return cr;
    }
    std::vector<float>& prev_v = t_deform_scratch.prev;   // 复用，不每帧分配
    std::vector<float>& next_v = t_deform_scratch.next;
    if (!ExpandDeformFrame(asset, data, tl_index, cur - 1u, deform_len, rigid ? setup : nullptr, prev_v) ||
        !ExpandDeformFrame(asset, data, tl_index, cur, deform_len, rigid ? setup : nullptr, next_v))
    {
        return Fail("apply: " + TlName(tl_index) + "（deform）的帧超出 deform 长度");
    }
    if (alpha == 1.0f)
    {
        if (blend == SpineGpuMixBlend::kAdd)
        {
            for (size_t i = 0; i < n; ++i)
            {
                const float p = prev_v[i];
                vertices[i] += rigid ? (p + (next_v[i] - p) * percent - setup[i]) : (p + (next_v[i] - p) * percent);
            }
        }
        else
        {
            // `__restrict`：`vertices` 是槽位的 deform 向量、prev/next 是 thread_local scratch，
            // 三者互不相交；不写这句 MSVC 不做向量化（实测这条循环 + 上面 `ExpandDeformFrame`
            // 里的 setup 累加一共值 3.0~4.7%，规格 §17.5）。
            float* __restrict v = vertices.data();
            const float* __restrict pv = prev_v.data();
            const float* __restrict nv = next_v.data();
            for (size_t i = 0; i < n; ++i)
            {
                const float p = pv[i];
                v[i] = p + (nv[i] - p) * percent;
            }
        }
        return Ok();
    }
    switch (blend)
    {
        case SpineGpuMixBlend::kSetup:
            for (size_t i = 0; i < n; ++i)
            {
                const float p = prev_v[i];
                if (rigid)
                {
                    const float s = setup[i];
                    vertices[i] = s + (p + (next_v[i] - p) * percent - s) * alpha;
                }
                else
                {
                    vertices[i] = (p + (next_v[i] - p) * percent) * alpha;
                }
            }
            break;
        case SpineGpuMixBlend::kFirst:
        case SpineGpuMixBlend::kReplace:
            for (size_t i = 0; i < n; ++i)
            {
                const float p = prev_v[i];
                vertices[i] += (p + (next_v[i] - p) * percent - vertices[i]) * alpha;
            }
            break;
        case SpineGpuMixBlend::kAdd:
            for (size_t i = 0; i < n; ++i)
            {
                const float p = prev_v[i];
                if (rigid)
                {
                    vertices[i] += (p + (next_v[i] - p) * percent - setup[i]) * alpha;
                }
                else
                {
                    vertices[i] += (p + (next_v[i] - p) * percent) * alpha;
                }
            }
            break;
    }
    return Ok();
}

}   // namespace

// =====================================================================
// 查找表
// =====================================================================

SpineImportResult SpineGpuBuildRuntimeData(const SpineGpuAsset& asset, SpineGpuRuntimeData& out)
{
    out = SpineGpuRuntimeData {};
    const uint32_t slot_count = asset.header.slot_count;
    if (asset.slots.size() != slot_count)
    {
        return Fail("runtime: header.slot_count 是 " + std::to_string(slot_count) + "，但 chunk 17 有 " +
                    std::to_string(asset.slots.size()) + " 条");
    }
    if (asset.bones.size() != asset.header.bone_count)
    {
        return Fail("runtime: header.bone_count 与 chunk 10 条数不一致");
    }
    out.blob = BuildSpineStringBlob(asset);

    // 名字规范化：同一个字符串在 chunk 1 里可能出现在多组（setup 附件名 / 动画附件名），
    // 偏移不同、身份相同（`Slot::setAttachment` 判的是附件对象，而对象由"槽位 + 名字"唯一确定）。
    std::unordered_map<std::string, uint32_t> name_ids;

    std::string err;
    const auto resolve = [&](uint32_t slot, uint32_t offset, SpineGpuAttachRef& ref) -> bool {
        ref = SpineGpuAttachRef {};
        if (offset == SpineGpuConstants::kNoAttachmentName)
        {
            return true;
        }
        const char* name = SpineGpuStringAt(out.blob, offset);
        if (name == nullptr)
        {
            err = "槽位 " + std::to_string(slot) + " 的附件名偏移 " + std::to_string(offset) + " 越界";
            return false;
        }
        if (name[0] == '\0')
        {
            return true;   // `attachmentName->length() == 0 ? NULL : ...`
        }
        auto [it, inserted] = name_ids.emplace(name, static_cast<uint32_t>(out.names.size()));
        if (inserted)
        {
            out.names.emplace_back(name);
        }
        ref.name_id = it->second;
        ref.name_offset = offset;
        (void)slot;
        // 这里只收名字；"这个名字在哪个皮肤里是哪条附件"等全部名字收齐、建好每个皮肤的表之后再解析
        // （见函数末尾）。名字找不到可画项 = 挂的是裁剪 / point / bbox / path，不是错。
        return true;
    };

    out.slot_setup.resize(slot_count);
    for (uint32_t s = 0; s < slot_count; ++s)
    {
        if (!resolve(s, asset.slots[s].setup_attachment_name_offset, out.slot_setup[s]))
        {
            return Fail("runtime: " + err);
        }
    }

    // ---- 绘制项的 deform 长度 ----
    out.item_deform_len.assign(asset.draw_items.size(), 0u);
    for (uint32_t i = 0; i < asset.draw_items.size(); ++i)
    {
        const SpineDrawItem& di = asset.draw_items[i];
        if (di.vertex_format == SpineVertexFormat::kRigid)
        {
            out.item_deform_len[i] = 2u * di.vertex_count;
            continue;
        }
        const size_t w0 = asset.weighted_segment_offset / 4u + static_cast<size_t>(di.vertex_base) * 2u;
        if ((w0 + static_cast<size_t>(di.vertex_count) * 2u) * 4u > asset.vertex_buffer.size())
        {
            return Fail("runtime: 绘制项 " + std::to_string(i) + " 的 weighted 顶点区间越过顶点缓冲");
        }
        uint32_t len = 0;
        for (uint32_t v = 0; v < di.vertex_count; ++v)
        {
            uint32_t cnt = 0;
            std::memcpy(&cnt, asset.vertex_buffer.data() + (w0 + v * 2u + 1u) * 4u, 4);
            len += 2u * cnt;
        }
        out.item_deform_len[i] = len;
    }

    // ---- 逐条时间轴 ----
    const size_t tl_count = asset.timelines.size();
    out.timeline_ref_first.assign(tl_count, 0u);
    out.draw_order_first.assign(tl_count, 0u);
    out.deform_frame_first.assign(tl_count, 0u);
    out.property_key.assign(tl_count, 0u);
    const std::vector<uint8_t>& payload = asset.timeline_payload;
    for (uint32_t ti = 0; ti < tl_count; ++ti)
    {
        const SpineGpuTimeline& tl = asset.timelines[ti];
        const SpineTimelineType type = static_cast<SpineTimelineType>(tl.type);
        FrameView fv;
        {
            const SpineImportResult fr = GetFrames(asset, ti, fv);
            if (!fr.ok)
            {
                return fr;
            }
        }
        // 属性身份：(类型, 目标)。drawOrder / event 全骨架只有一个属性（参考实现不加目标）。
        const uint64_t target = (type == SpineTimelineType::kDrawOrder || type == SpineTimelineType::kEvent)
                                    ? 0u
                                    : static_cast<uint64_t>(tl.target);
        out.property_key[ti] = (static_cast<uint64_t>(tl.type) << 32) | target;

        if (type == SpineTimelineType::kAttachment)
        {
            if (tl.target >= slot_count)
            {
                return Fail("runtime: " + TlName(ti) + "（attachment）的槽位下标越界");
            }
            if (tl.payload_count < tl.frame_count ||
                static_cast<size_t>(tl.payload_offset) + static_cast<size_t>(tl.frame_count) * 4u > payload.size())
            {
                return Fail("runtime: " + TlName(ti) + "（attachment）的载荷越界");
            }
            out.timeline_ref_first[ti] = static_cast<uint32_t>(out.attach_refs.size());
            for (uint32_t f = 0; f < tl.frame_count; ++f)
            {
                SpineGpuAttachRef ref;
                if (!resolve(tl.target, GetU32At(payload, static_cast<size_t>(tl.payload_offset) + f * 4u), ref))
                {
                    return Fail("runtime: " + TlName(ti) + ": " + err);
                }
                out.attach_refs.push_back(ref);
            }
            continue;
        }

        if (type == SpineTimelineType::kDrawOrder)
        {
            out.draw_order_first[ti] = static_cast<uint32_t>(out.draw_order_frame_pos.size());
            size_t pos = tl.payload_offset;
            for (uint32_t f = 0; f < tl.frame_count; ++f)
            {
                if (pos + 4u > payload.size())
                {
                    return Fail("runtime: " + TlName(ti) + "（drawOrder）的载荷被截断");
                }
                const uint32_t n = GetU32At(payload, pos);
                pos += 4u;
                if (pos + static_cast<size_t>(n) * 8u > payload.size())
                {
                    return Fail("runtime: " + TlName(ti) + "（drawOrder）的载荷越界");
                }
                if (n == 0)
                {
                    out.draw_order_frame_pos.push_back(SpineGpuAttachRef::kNone);
                    continue;
                }
                // ---- 重建完整排列（SkeletonBinary.cpp:1040-1072 / SkeletonJson.cpp:1129-1165）----
                // ⚠️ 三处不对称，每一处抄错都能得到一个看起来合理的排列：
                //   a) `drawOrder[originalIndex + offset] = originalIndex` —— 下标是**原始（setup）
                //      下标**，offset **有符号**；
                //   b) 未列出的槽位收进 `unchanged`，**按槽位升序**收集；
                //   c) 回填时从**数组末尾往前**填，`unchanged` 也从**末尾往前**取。
                std::vector<int32_t> order(static_cast<size_t>(slot_count), -1);
                std::vector<int32_t> unchanged;
                unchanged.reserve(slot_count);
                uint32_t original_index = 0;
                for (uint32_t k = 0; k < n; ++k)
                {
                    const uint32_t slot_index = GetU32At(payload, pos + static_cast<size_t>(k) * 8u);
                    const int32_t offset = static_cast<int32_t>(GetU32At(payload, pos + static_cast<size_t>(k) * 8u + 4u));
                    if (slot_index >= slot_count || slot_index < original_index)
                    {
                        return Fail("runtime: " + TlName(ti) +
                                    "（drawOrder）的槽位下标不是升序（参考实现在这里是死循环）");
                    }
                    while (original_index != slot_index)
                    {
                        unchanged.push_back(static_cast<int32_t>(original_index));
                        ++original_index;
                    }
                    const int64_t to = static_cast<int64_t>(original_index) + offset;
                    if (to < 0 || to >= static_cast<int64_t>(slot_count))
                    {
                        return Fail("runtime: " + TlName(ti) + "（drawOrder）把槽位 " + std::to_string(original_index) +
                                    " 挪到了越界位置 " + std::to_string(to));
                    }
                    order[static_cast<size_t>(to)] = static_cast<int32_t>(original_index);
                    ++original_index;
                }
                while (original_index < slot_count)
                {
                    unchanged.push_back(static_cast<int32_t>(original_index));
                    ++original_index;
                }
                size_t ui = unchanged.size();
                for (int32_t ii = static_cast<int32_t>(slot_count) - 1; ii >= 0; --ii)
                {
                    if (order[static_cast<size_t>(ii)] == -1)
                    {
                        if (ui == 0)
                        {
                            return Fail("runtime: " + TlName(ti) + "（drawOrder）的排列重建失败（空洞比未列出槽位多）");
                        }
                        order[static_cast<size_t>(ii)] = unchanged[--ui];
                    }
                }
                out.draw_order_frame_pos.push_back(static_cast<uint32_t>(out.draw_order_pool.size()));
                for (uint32_t i = 0; i < slot_count; ++i)
                {
                    out.draw_order_pool.push_back(static_cast<uint32_t>(order[i]));
                }
                pos += static_cast<size_t>(n) * 8u;
            }
            continue;
        }

        if (type == SpineTimelineType::kDeform)
        {
            out.deform_frame_first[ti] = static_cast<uint32_t>(out.deform_frame_pos.size());
            size_t pos = tl.payload_offset;
            for (uint32_t f = 0; f < tl.frame_count; ++f)
            {
                if (pos + 8u > payload.size())
                {
                    return Fail("runtime: " + TlName(ti) + "（deform）的载荷被截断");
                }
                out.deform_frame_pos.push_back(static_cast<uint32_t>(pos));
                const uint32_t count = GetU32At(payload, pos + 4u);
                pos += 8u + static_cast<size_t>(count) * 4u;
                if (pos > payload.size())
                {
                    return Fail("runtime: " + TlName(ti) + "（deform）的载荷被截断");
                }
            }
            continue;
        }

        if (type == SpineTimelineType::kRotate || type == SpineTimelineType::kTranslate ||
            type == SpineTimelineType::kScale || type == SpineTimelineType::kShear)
        {
            if (tl.target >= asset.bones.size())
            {
                return Fail("runtime: " + TlName(ti) + "的骨骼下标越界");
            }
        }
        else if (type == SpineTimelineType::kColor || type == SpineTimelineType::kTwoColor)
        {
            if (tl.target >= slot_count)
            {
                return Fail("runtime: " + TlName(ti) + "（颜色）的槽位下标越界");
            }
        }
        else if (type == SpineTimelineType::kPathConstraintPosition || type == SpineTimelineType::kPathConstraintMix ||
                 type == SpineTimelineType::kPathConstraintSpacing)
        {
            if (tl.target >= asset.paths.size())
            {
                return Fail("runtime: " + TlName(ti) + "的 path 约束下标越界");
            }
        }
    }

    // ---- 每个皮肤的 (槽位, 名字) → 附件表（`Skin::getAttachment`）----
    const uint32_t skin_count = static_cast<uint32_t>(asset.skin_names.size());
    out.has_default_skin = skin_count > 0 && asset.skin_names[SpineGpuConstants::kDefaultSkinId] == "default";
    out.skin_hits.assign(skin_count, {});
    const auto key = [](uint32_t slot, uint32_t name_id) { return (static_cast<uint64_t>(slot) << 32) | name_id; };
    const auto name_id_of = [&](const std::string& n, uint32_t& id) {
        const auto it = name_ids.find(n);
        if (it == name_ids.end())
        {
            return false;   // 没有任何 setup / 时间轴引用这个名字 ⇒ 槽位不可能挂它
        }
        id = it->second;
        return true;
    };
    for (uint32_t i = 0; i < asset.draw_items.size(); ++i)
    {
        const SpineDrawItem& di = asset.draw_items[i];
        uint32_t id = 0;
        if (di.skin_id >= skin_count || di.slot_index >= slot_count || di.attachment_id >= asset.attachment_names.size())
        {
            return Fail("runtime: 绘制项 " + std::to_string(i) + " 的皮肤 / 槽位 / 附件下标越界");
        }
        if (name_id_of(asset.attachment_names[di.attachment_id], id))
        {
            SpineGpuRuntimeData::SkinHit& h = out.skin_hits[di.skin_id][key(di.slot_index, id)];
            if (h.draw_item == SpineGpuAttachRef::kNone)
            {
                h.draw_item = i;   // 同一 (皮肤, 槽位, 名字) 只有一条附件；保险起见取第一个
            }
        }
    }
    // path 按 (皮肤, 槽位, 名字) 找（一个槽位挂两条 path 是合法的，实测 6 / 782）
    for (uint32_t k = 0; k < asset.path_attachments.size() && k < asset.path_attachment_names.size(); ++k)
    {
        const SpineGpuPathAttachment& pa = asset.path_attachments[k];
        uint32_t id = 0;
        if (pa.skin_index < skin_count && name_id_of(asset.path_attachment_names[k], id))
        {
            SpineGpuRuntimeData::SkinHit& h = out.skin_hits[pa.skin_index][key(pa.slot_index, id)];
            if (h.path_att == SpineGpuConstants::kNoPathAttachment)
            {
                h.path_att = k;
            }
        }
    }
    // 裁剪附件同理（v15，chunk 9 带名字）
    for (uint32_t k = 0; k < asset.clip_polygons.size() && k < asset.clip_names.size(); ++k)
    {
        const SpineGpuClipPolygon& c = asset.clip_polygons[k];
        uint32_t id = 0;
        if (c.skin_id < skin_count && name_id_of(asset.clip_names[k], id))
        {
            SpineGpuRuntimeData::SkinHit& h = out.skin_hits[c.skin_id][key(c.slot_index, id)];
            if (h.clip == SpineGpuAttachRef::kNone)
            {
                h.clip = k;
            }
        }
    }

    // ---- 查找表里的引用按"没有当前皮肤"解析（= 只看 default 皮肤）----
    for (uint32_t sl = 0; sl < slot_count; ++sl)
    {
        out.slot_setup[sl] = SpineGpuResolveAttachment(out, kSpineGpuNoSkin, sl, out.slot_setup[sl]);
    }
    for (uint32_t ti = 0; ti < tl_count; ++ti)
    {
        const SpineGpuTimeline& tl = asset.timelines[ti];
        if (static_cast<SpineTimelineType>(tl.type) != SpineTimelineType::kAttachment)
        {
            continue;
        }
        for (uint32_t f = 0; f < tl.frame_count; ++f)
        {
            SpineGpuAttachRef& r = out.attach_refs[static_cast<size_t>(out.timeline_ref_first[ti]) + f];
            r = SpineGpuResolveAttachment(out, kSpineGpuNoSkin, tl.target, r);
        }
    }
    return Ok();
}

SpineGpuAttachRef SpineGpuResolveAttachment(const SpineGpuRuntimeData& data, uint32_t skin, uint32_t slot,
                                            const SpineGpuAttachRef& ref)
{
    SpineGpuAttachRef out;
    out.name_id = ref.name_id;
    out.name_offset = ref.name_offset;
    if (ref.name_id == SpineGpuAttachRef::kNone)
    {
        return out;
    }
    const uint64_t k = (static_cast<uint64_t>(slot) << 32) | ref.name_id;
    const auto take = [&](uint32_t s) {
        if (s >= data.skin_hits.size())
        {
            return false;
        }
        const auto it = data.skin_hits[s].find(k);
        if (it == data.skin_hits[s].end())
        {
            return false;
        }
        out.skin = s;
        out.draw_item = it->second.draw_item;
        out.path_att = it->second.path_att;
        out.clip = it->second.clip;
        return true;
    };
    // 先当前皮肤、再 default 皮肤（Skeleton.cpp:338-345）
    if (skin != kSpineGpuNoSkin && take(skin))
    {
        return out;
    }
    if (data.has_default_skin)
    {
        take(SpineGpuConstants::kDefaultSkinId);
    }
    return out;
}

bool SpineGpuFindSkin(const SpineGpuAsset& asset, const std::string& name, uint32_t& out)
{
    for (uint32_t i = 0; i < asset.skin_names.size(); ++i)
    {
        if (asset.skin_names[i] == name)
        {
            out = i;
            return true;
        }
    }
    return false;
}

SpineImportResult SpineGpuSetSkin(const SpineGpuAsset& asset, const SpineGpuRuntimeData& data,
                                  SpineGpuSkeletonState& state, uint32_t skin)
{
    if (skin != kSpineGpuNoSkin && skin >= data.skin_hits.size())
    {
        return Fail("setSkin: 皮肤下标 " + std::to_string(skin) + " 越界（只有 " +
                    std::to_string(data.skin_hits.size()) + " 个皮肤）");
    }
    if (state.slots.size() != data.slot_setup.size())
    {
        return Fail("setSkin: 状态与查找表的槽位数不一致");
    }
    (void)asset;
    if (skin != kSpineGpuNoSkin)
    {
        const auto& table = data.skin_hits[skin];
        for (uint32_t sl = 0; sl < state.slots.size(); ++sl)
        {
            SpineGpuSlotState& slot = state.slots[sl];
            uint32_t name_id = SpineGpuAttachRef::kNone;
            if (state.skin != kSpineGpuNoSkin)
            {
                // attachAll（Skin.cpp:134-149）：只换"当前挂的正是旧皮肤里那一条"的槽位
                if (slot.attachment.skin != state.skin)
                {
                    continue;
                }
                name_id = slot.attachment.name_id;
            }
            else
            {
                // 旧皮肤为空：按 setup 附件名（Skeleton.cpp:310-324）
                name_id = data.slot_setup[sl].name_id;
            }
            if (name_id == SpineGpuAttachRef::kNone ||
                table.find((static_cast<uint64_t>(sl) << 32) | name_id) == table.end())
            {
                continue;   // 新皮肤里没有这个名字 ⇒ 不动（**不**回落 default）
            }
            SpineGpuAttachRef want;
            want.name_id = name_id;
            want.name_offset = state.skin != kSpineGpuNoSkin ? slot.attachment.name_offset
                                                            : data.slot_setup[sl].name_offset;
            // 新皮肤里一定命中 ⇒ Resolve 给的就是新皮肤那条
            SpineGpuSetSlotAttachment(slot, SpineGpuResolveAttachment(data, skin, sl, want));
        }
    }
    state.skin = skin;
    return Ok();
}

// =====================================================================
// 状态
// =====================================================================

void SpineGpuSetSlotAttachment(SpineGpuSlotState& slot, const SpineGpuAttachRef& ref)
{
    if (slot.attachment.name_id == ref.name_id && slot.attachment.skin == ref.skin)
    {
        return;
    }
    slot.attachment = ref;
    slot.deform.clear();
}

void SpineGpuSetBonesToSetupPose(const SpineGpuAsset& asset, SpineGpuSkeletonState& state)
{
    // `Bone::setToSetupPose()`（Bone.cpp:208-217）：只动**局部**值，applied / 世界不动。
    for (size_t i = 0; i < state.bones.size() && i < asset.bones.size(); ++i)
    {
        const SpineGpuBone& b = asset.bones[i];
        SpineGpuBoneState& s = state.bones[i];
        s.x = b.x;
        s.y = b.y;
        s.rotation = b.rotation;
        s.scale_x = b.scale_x;
        s.scale_y = b.scale_y;
        s.shear_x = b.shear_x;
        s.shear_y = b.shear_y;
    }
    // 约束运行时参数（IK / transform 的恒等于 setup，没有存 —— 见头文件）
    for (size_t i = 0; i < asset.paths.size() && i < state.path_position.size(); ++i)
    {
        state.path_position[i] = asset.paths[i].position;
        state.path_spacing[i] = asset.paths[i].spacing;
        state.path_rotate_mix[i] = asset.paths[i].rotate_mix;
        state.path_translate_mix[i] = asset.paths[i].translate_mix;
    }
}

namespace
{
// `Slot::setToSetupPose()`（Slot.cpp:54-64）
void SlotToSetupPose(const SpineGpuAsset& asset, const SpineGpuRuntimeData& data, SpineGpuSkeletonState& state,
                     uint32_t s)
{
    const SpineGpuSlot& d = asset.slots[s];
    SpineGpuSlotState& slot = state.slots[s];
    ColorSet(slot.color, d.color_r, d.color_g, d.color_b, d.color_a);
    // 暗色**不复位**（参考实现就是这样，见头文件第 2 条）
    // `_skeleton.getAttachment(index, name)` ⇒ 按当前皮肤解析
    const SpineGpuAttachRef setup = SpineGpuResolveAttachment(data, state.skin, s, data.slot_setup[s]);
    if (setup.name_id != SpineGpuAttachRef::kNone)
    {
        // `_attachment = NULL; setAttachment(...)` —— 先置空再挂 ⇒ 判据必不等 ⇒ deform **必清**
        slot.attachment = SpineGpuAttachRef {};
        SpineGpuSetSlotAttachment(slot, setup);
    }
    else
    {
        SpineGpuSetSlotAttachment(slot, SpineGpuAttachRef {});
    }
}
}   // namespace

void SpineGpuSetSlotsToSetupPose(const SpineGpuAsset& asset, const SpineGpuRuntimeData& data,
                                 SpineGpuSkeletonState& state)
{
    // `Skeleton::setSlotsToSetupPose()`（Skeleton.cpp:261-270）：先绘制顺序、再逐槽位
    ResetDrawOrder(state);
    for (uint32_t s = 0; s < state.slots.size(); ++s)
    {
        SlotToSetupPose(asset, data, state, s);
    }
}

void SpineGpuSetToSetupPose(const SpineGpuAsset& asset, const SpineGpuRuntimeData& data, SpineGpuSkeletonState& state)
{
    SpineGpuSetBonesToSetupPose(asset, state);
    SpineGpuSetSlotsToSetupPose(asset, data, state);
}

SpineImportResult SpineGpuInitSkeletonState(const SpineGpuAsset& asset, const SpineGpuRuntimeData& data,
                                            SpineGpuSkeletonState& out)
{
    const uint32_t bone_count = asset.header.bone_count;
    const uint32_t slot_count = asset.header.slot_count;
    if (asset.bones.size() != bone_count || asset.slots.size() != slot_count || data.slot_setup.size() != slot_count)
    {
        return Fail("init: 骨骼 / 槽位条数与 header 不一致（或查找表不是用这个资产建的）");
    }
    out = SpineGpuSkeletonState {};
    out.bones.resize(bone_count);
    for (uint32_t i = 0; i < bone_count; ++i)
    {
        const SpineGpuBone& b = asset.bones[i];
        SpineGpuBoneState& s = out.bones[i];
        if (b.parent >= 0 && static_cast<uint32_t>(b.parent) >= i)
        {
            return Fail("init: 第 " + std::to_string(i) + " 根骨骼的 parent 是 " + std::to_string(b.parent) +
                        "，不是 < 自己的下标（正向遍历就不是拓扑序了）");
        }
        s.length = b.length;
        s.parent = b.parent;
        s.mode = b.transform_mode;
    }
    out.path_position.assign(asset.paths.size(), 0.0f);
    out.path_spacing.assign(asset.paths.size(), 0.0f);
    out.path_rotate_mix.assign(asset.paths.size(), 0.0f);
    out.path_translate_mix.assign(asset.paths.size(), 0.0f);
    SpineGpuSetBonesToSetupPose(asset, out);   // Bone 构造函数里那次 + PathConstraint 构造取 data 值

    // `Slot::Slot()`（Slot.cpp:42-52）：color(1,1,1,1)、darkColor(0,0,0,0)、attachment NULL，再 setToSetupPose
    out.slots.assign(slot_count, SpineGpuSlotState {});
    out.draw_order.resize(slot_count);
    ResetDrawOrder(out);
    for (uint32_t s = 0; s < slot_count; ++s)
    {
        SlotToSetupPose(asset, data, out, s);
    }
    return Ok();
}

// =====================================================================
// apply
// =====================================================================

SpineImportResult SpineGpuApplyTimeline(const SpineGpuAsset& asset, const SpineGpuRuntimeData& data,
                                        SpineGpuSkeletonState& state, uint32_t tl_index, float last_time, float time,
                                        float alpha, SpineGpuMixBlend blend, SpineGpuMixDirection direction)
{
    (void)last_time;   // 只有 event 时间轴读它，event 不改状态
    FrameView fv;
    {
        const SpineImportResult fr = GetFrames(asset, tl_index, fv);
        if (!fr.ok)
        {
            return fr;
        }
    }
    const SpineGpuTimeline& tl = asset.timelines[tl_index];
    switch (static_cast<SpineTimelineType>(tl.type))
    {
        case SpineTimelineType::kRotate:
            return ApplyRotate(asset, state, tl_index, fv, time, alpha, blend);
        case SpineTimelineType::kTranslate:
            return ApplyTranslateOrShear(asset, state, tl_index, fv, time, alpha, blend, false);
        case SpineTimelineType::kShear:
            return ApplyTranslateOrShear(asset, state, tl_index, fv, time, alpha, blend, true);
        case SpineTimelineType::kScale:
            return ApplyScale(asset, state, tl_index, fv, time, alpha, blend, direction);
        case SpineTimelineType::kColor:
            return ApplyColor(asset, state, tl_index, fv, time, alpha, blend, false);
        case SpineTimelineType::kTwoColor:
            return ApplyColor(asset, state, tl_index, fv, time, alpha, blend, true);
        case SpineTimelineType::kAttachment:
            return ApplyAttachment(asset, data, state, tl_index, fv, time, blend, direction);
        case SpineTimelineType::kDrawOrder:
            return ApplyDrawOrder(data, state, tl_index, fv, time, blend, direction);
        case SpineTimelineType::kDeform:
            return ApplyDeform(asset, data, state, tl_index, fv, time, alpha, blend);
        case SpineTimelineType::kPathConstraintPosition:
            return ApplyPathPosition(asset, state, tl_index, fv, time, alpha, blend);
        case SpineTimelineType::kPathConstraintMix:
            return ApplyPathMix(asset, state, tl_index, fv, time, alpha, blend);
        case SpineTimelineType::kEvent:
            return Ok();   // 事件不改渲染状态（本实现不派发事件）
        case SpineTimelineType::kIkConstraint:
        case SpineTimelineType::kTransformConstraint:
        case SpineTimelineType::kPathConstraintSpacing:
            // 这三类**会改姿势**，本阶段不做 ⇒ 显式报错，不能静默跳过
            // （实测 0 / 782 个资产用到，所以不会真的挡住任何资产）。
            return Fail("apply: " + TlName(tl_index) + "类型 " + std::to_string(tl.type) +
                        " 本阶段不做（会改姿势，跳过会得到静默错的姿势）");
    }
    return Fail("apply: " + TlName(tl_index) + "类型 " + std::to_string(tl.type) + " 不认识");
}

SpineImportResult SpineGpuApplyAnimation(const SpineGpuAsset& asset, const SpineGpuRuntimeData& data,
                                         SpineGpuSkeletonState& state, uint32_t anim_index, float last_time,
                                         float time, bool loop, float alpha, SpineGpuMixBlend blend,
                                         SpineGpuMixDirection direction)
{
    if (anim_index >= asset.anims.size())
    {
        return Fail("apply: 动画下标 " + std::to_string(anim_index) + " 越界（只有 " +
                    std::to_string(asset.anims.size()) + " 个动画）");
    }
    const SpineGpuAnim& anim = asset.anims[anim_index];
    if (loop && anim.duration != 0.0f)
    {
        time = std::fmod(time, anim.duration);
        if (last_time > 0.0f)
        {
            last_time = std::fmod(last_time, anim.duration);
        }
    }
    for (uint32_t ti = 0; ti < anim.timeline_count; ++ti)
    {
        const SpineImportResult r = SpineGpuApplyTimeline(asset, data, state, anim.timeline_first + ti, last_time,
                                                          time, alpha, blend, direction);
        if (!r.ok)
        {
            return r;
        }
    }
    return Ok();
}

SpineImportResult SpineGpuApplyRotateTimelineMixed(const SpineGpuAsset& asset, SpineGpuSkeletonState& state,
                                                   uint32_t tl_index, float time, float alpha, SpineGpuMixBlend blend,
                                                   float* timelines_rotation, bool first_frame)
{
    if (first_frame)
    {
        timelines_rotation[0] = 0.0f;
    }
    FrameView fv;
    {
        const SpineImportResult fr = GetFrames(asset, tl_index, fv);
        if (!fr.ok)
        {
            return fr;
        }
    }
    if (alpha == 1.0f)
    {
        // `rotateTimeline->apply(skeleton, 0, time, NULL, 1, blend, MixDirection_In)`（:642-645）
        return ApplyRotate(asset, state, tl_index, fv, time, 1.0f, blend);
    }

    const SpineGpuTimeline& tl = asset.timelines[tl_index];
    if (tl.target >= state.bones.size())
    {
        return Fail("apply: " + TlName(tl_index) + "（rotate）的骨骼下标越界");
    }
    SpineGpuBoneState& bone = state.bones[tl.target];
    const float setup = asset.bones[tl.target].rotation;
    float r1;
    float r2;
    if (time < fv.FirstTime())
    {
        // ⚠️ 照抄参考实现的 switch 贯穿（:651-659）：Setup 写完**贯穿进 default 直接 return**；
        //    只有 First 走到下面的混合。
        switch (blend)
        {
            case SpineGpuMixBlend::kSetup:
                bone.rotation = setup;
                return Ok();
            case SpineGpuMixBlend::kFirst:
                r1 = bone.rotation;
                r2 = setup;
                break;
            default:
                return Ok();
        }
    }
    else
    {
        r1 = blend == SpineGpuMixBlend::kSetup ? setup : bone.rotation;
        if (time >= fv.LastTime())
        {
            r2 = setup + fv.Last(1);
        }
        else
        {
            const uint32_t cur = SpineGpuBinarySearchFrames(fv.frames, fv.frame_count, fv.entries, time);
            const float prev_rotation = fv.frames[static_cast<size_t>(cur) - 1u];
            float percent = 0.0f;
            const SpineImportResult cr = SpineGpuCurvePercentAt(asset, tl, tl_index, cur, time, percent);
            if (!cr.ok)
            {
                return cr;
            }
            r2 = fv.frames[cur + 1u] - prev_rotation;
            r2 = SpineGpuWrapDegrees(r2);
            r2 = prev_rotation + r2 * percent + setup;
            r2 = SpineGpuWrapDegrees(r2);
        }
    }

    // "首帧按最短路径定方向，之后检测跨零"（:681-714）
    float total;
    float diff = r2 - r1;
    diff = SpineGpuWrapDegrees(diff);
    if (diff == 0.0f)
    {
        total = timelines_rotation[0];
    }
    else
    {
        float last_total;
        float last_diff;
        if (first_frame)
        {
            last_total = 0.0f;
            last_diff = diff;
        }
        else
        {
            last_total = timelines_rotation[0];   // 混合的角度与方向（含整圈）
            last_diff = timelines_rotation[1];    // 两根骨骼之差
        }
        const bool current = diff > 0.0f;
        bool dir = last_total >= 0.0f;
        // 跨 0（不是 180）才算换向
        if (Sign(last_diff) != Sign(diff) && Abs(last_diff) <= 90.0f)
        {
            // 转过 360 之后的跨零是"绕圈"
            if (Abs(last_total) > 180.0f)
            {
                last_total += 360.0f * Sign(last_total);
            }
            dir = current;
        }
        // `MathUtil::fmod` = `(float)::fmod(a, b)`；fmod 的结果在 float 上精确可表示 ⇒ float 版逐位相同
        total = diff + last_total - std::fmod(last_total, 360.0f);
        if (dir != current)
        {
            total += 360.0f * Sign(last_total);
        }
        timelines_rotation[0] = total;
    }
    timelines_rotation[1] = diff;
    r1 += total * alpha;
    bone.rotation = SpineGpuWrapDegrees(r1);
    return Ok();
}
