#include "Runtime/Function/Render/Spine/SpineGpuAnimSolver.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

// =====================================================================
// 参考实现：G:/OSGame/unity-engine-source/Modules/Spine/spine-cpp
//   * Animation::binarySearch()          Animation.cpp:84-106
//   * CurveTimeline::getCurvePercent()   CurveTimeline.cpp:110-141
//   * CurveTimeline::setCurve()          CurveTimeline.cpp:78-105（前向差分表）
//   * ColorTimeline::apply()             ColorTimeline.cpp:64-113
//   * TwoColorTimeline::apply()          TwoColorTimeline.cpp:68-149
//   * AttachmentTimeline::apply()        AttachmentTimeline.cpp:53-80
//   * DrawOrderTimeline::apply()         DrawOrderTimeline.cpp:53-97
//   * drawOrder 帧的重建                 SkeletonBinary.cpp:1040-1072 / SkeletonJson.cpp:1129-1165
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

uint32_t GetU32At(const std::vector<uint8_t>& bytes, size_t pos)
{
    return static_cast<uint32_t>(bytes[pos]) | (static_cast<uint32_t>(bytes[pos + 1]) << 8) |
           (static_cast<uint32_t>(bytes[pos + 2]) << 16) | (static_cast<uint32_t>(bytes[pos + 3]) << 24);
}

}   // namespace

// ---- 从这里起是导出的（骨骼侧解算 `SpineGpuPoseSolver` 要用同一份实现）----
// `Animation::binarySearch()` —— 见头文件上的注释。
uint32_t SpineGpuBinarySearchFrames(const float* frames, uint32_t count, uint32_t step, float target)
{
    int32_t low = 0;
    int32_t high = static_cast<int32_t>(count) - 2;
    if (high == 0)
    {
        return step;
    }
    int32_t current = static_cast<int32_t>(static_cast<uint32_t>(high) >> 1);
    while (true)
    {
        if (frames[static_cast<size_t>(current + 1) * step] <= target)
        {
            low = current + 1;
        }
        else
        {
            high = current;
        }
        if (low == high)
        {
            return static_cast<uint32_t>(low + 1) * step;
        }
        current = static_cast<int32_t>(static_cast<uint32_t>(low + high) >> 1);
    }
}

SpineImportResult SpineGpuCurvePercentAt(const SpineGpuAsset& asset, const SpineGpuTimeline& tl, uint32_t tl_index,
                                         uint32_t cur, float time, float& percent_out)
{
    const uint32_t entries = SpineTimelineEntries(static_cast<SpineTimelineType>(tl.type));
    if (entries == 0 || cur < entries)
    {
        return Fail("sample: 第 " + std::to_string(tl_index) + " 条时间轴的帧定位结果不自洽");
    }
    // `cur` 是**相对本时间轴帧数组起点**的下标（BinarySearchFrames 的口径）。
    const uint64_t abs_cur = static_cast<uint64_t>(tl.frame_first) + cur;
    const uint64_t abs_prev = static_cast<uint64_t>(tl.frame_first) + (cur - entries);
    if (abs_cur >= asset.frames.size() || abs_prev >= asset.frames.size())
    {
        return Fail("sample: 第 " + std::to_string(tl_index) + " 条时间轴的帧区间越界");
    }
    const float ft = asset.frames[static_cast<size_t>(abs_cur)];
    const float pt = asset.frames[static_cast<size_t>(abs_prev)];
    // ⚠️ 逐字照抄参考实现的写法 `1 - (time - frameTime) / (prevTime - frameTime)`
    //    （RotateTimeline.cpp:101-102、TranslateTimeline.cpp:96-97、DeformTimeline.cpp:186 …
    //    所有 CurveTimeline 都是这一句）。`(time - pt) / (ft - pt)` 数学上相等，浮点结果差
    //    最后 1-2 ulp；经过 IK / transform 约束链放大后，OSGame 的 `UI_Yixing_Spine`
    //    就因此超出 1e-4 的对拍容差（唯一一个对不上的资产）。
    float percent = 1.0f - (time - ft) / (pt - ft);

    // 段号 = 前一帧的下标（`ColorTimeline.cpp:93` `frame / ENTRIES - 1`）
    const uint32_t segment = cur / entries - 1u;
    if (static_cast<size_t>(tl.curve_first) + segment >= asset.curve_types.size())
    {
        return Fail("sample: 第 " + std::to_string(tl_index) + " 条时间轴的曲线段下标越界");
    }
    const SpineCurveType curve =
        static_cast<SpineCurveType>(asset.curve_types[static_cast<size_t>(tl.curve_first) + segment]);
    const SpineGpuBezier* bez = nullptr;
    if (curve == SpineCurveType::kBezier)
    {
        // bezier 是按段序**紧凑**排的 ⇒ 这一段之前标了 bezier 的段数就是下标。
        uint32_t nth = 0;
        for (uint32_t s = 0; s < segment; ++s)
        {
            if (asset.curve_types[static_cast<size_t>(tl.curve_first) + s] ==
                static_cast<uint8_t>(SpineCurveType::kBezier))
            {
                ++nth;
            }
        }
        if (nth >= tl.bezier_count)
        {
            return Fail("sample: 第 " + std::to_string(tl_index) + " 条时间轴第 " + std::to_string(segment) +
                        " 段标了 bezier 但控制点不够");
        }
        const size_t bi = static_cast<size_t>(tl.bezier_first) + nth;
        if (bi >= asset.beziers.size())
        {
            return Fail("sample: 第 " + std::to_string(tl_index) + " 条时间轴的 bezier 下标越界");
        }
        bez = &asset.beziers[bi];
    }
    percent_out = SpineGpuCurvePercent(curve, bez, percent);
    SpineImportResult r;
    r.ok = true;
    return r;
}

// bezier 查找表的重建：`CurveTimeline::setCurve()`（:78-105）。
// 输出 9 个采样点（t = 0.1 .. 0.9），对应参考实现的 `BEZIER_VALUES = 10*2-2 = 18` 个 float。
//
// ⚠️ 4 个常数 `0.03f / 0.006f / 0.3f / 0.16666667f` **照抄**：
//    它们是"步长 0.1 的三次 bezier 前向差分"的系数，不是可以重排的算术式，
//    化简即错且不报错（曲线会悄悄变成另一条）。
void BuildBezierTable(const SpineGpuBezier& b, float* xs, float* ys)
{
    const float tmpx = (-b.cx1 * 2.0f + b.cx2) * 0.03f;
    const float tmpy = (-b.cy1 * 2.0f + b.cy2) * 0.03f;
    const float dddfx = ((b.cx1 - b.cx2) * 3.0f + 1.0f) * 0.006f;
    const float dddfy = ((b.cy1 - b.cy2) * 3.0f + 1.0f) * 0.006f;
    float ddfx = tmpx * 2.0f + dddfx;
    float ddfy = tmpy * 2.0f + dddfy;
    float dfx = b.cx1 * 0.3f + tmpx + dddfx * 0.16666667f;
    float dfy = b.cy1 * 0.3f + tmpy + dddfy * 0.16666667f;
    float x = dfx;
    float y = dfy;
    for (int n = 0; n < 9; ++n)
    {
        xs[n] = x;
        ys[n] = y;
        dfx += ddfx;
        dfy += ddfy;
        ddfx += dddfx;
        ddfy += dddfy;
        x += dfx;
        y += dfy;
    }
}

float SpineGpuCurvePercent(SpineCurveType type, const SpineGpuBezier* bezier, float percent)
{
    // `MathUtil::clamp(percent, 0, 1)`（CurveTimeline.cpp:112）—— 参考实现先夹一遍。
    if (percent < 0.0f)
    {
        percent = 0.0f;
    }
    else if (percent > 1.0f)
    {
        percent = 1.0f;
    }

    if (type == SpineCurveType::kStepped)
    {
        return 0.0f;
    }
    if (type != SpineCurveType::kBezier)
    {
        return percent;   // kLinear
    }
    if (bezier == nullptr)
    {
        return percent;   // 容器不自洽（标了 bezier 却没有控制点）—— 读端已经报过了
    }

    float xs[9];
    float ys[9];
    BuildBezierTable(*bezier, xs, ys);

    // `getCurvePercent()`（:118-141）：找第一个 x >= percent 的点，与前一点线性插值。
    float x = 0.0f;
    for (int n = 0; n < 9; ++n)
    {
        x = xs[n];
        if (x >= percent)
        {
            const float prev_x = (n == 0) ? 0.0f : xs[n - 1];
            const float prev_y = (n == 0) ? 0.0f : ys[n - 1];
            return prev_y + (ys[n] - prev_y) * (percent - prev_x) / (x - prev_x);
        }
    }
    // 落在最后一个采样点之后：与 (1,1) 连线。
    return ys[8] + (1.0f - ys[8]) * (percent - x) / (1.0f - x);
}

std::vector<uint8_t> BuildSpineStringBlob(const SpineGpuAsset& asset)
{
    std::vector<uint8_t> blob;
    std::vector<uint32_t> slot_off;
    std::vector<uint32_t> anim_off;
    std::vector<uint32_t> anim_att_off;
    std::vector<uint32_t> path_att_off;
    BuildSpineStringTable(asset, blob, slot_off, anim_off, anim_att_off, path_att_off);
    return blob;
}

const char* SpineGpuStringAt(const std::vector<uint8_t>& blob, uint32_t offset)
{
    if (offset >= blob.size())
    {
        return nullptr;
    }
    return reinterpret_cast<const char*>(blob.data() + offset);
}

SpineImportResult SpineGpuSampleSlots(const SpineGpuAsset& asset, uint32_t anim_index, float time,
                                      SpineGpuSlotSampleResult& out)
{
    if (anim_index >= asset.anims.size())
    {
        return Fail("sample: 动画下标 " + std::to_string(anim_index) + " 越界（只有 " +
                    std::to_string(asset.anims.size()) + " 个动画）");
    }
    const SpineGpuAnim& anim = asset.anims[anim_index];

    const uint32_t slot_count = asset.header.slot_count;
    if (slot_count != asset.slots.size())
    {
        return Fail("sample: header.slot_count 是 " + std::to_string(slot_count) + "，但 chunk 17 有 " +
                    std::to_string(asset.slots.size()) + " 条（读端应该在读容器时就报这个）");
    }

    // ---- 复位到 setup ----
    // 这一步不是"初始化"，它就是 `Slot::setToSetupPose()` 的语义：
    // 颜色 = `SlotData::getColor()`、暗色 = `SlotData::getDarkColor()`、
    // 附件 = `SlotData::getAttachmentName()`、绘制顺序 = 槽位下标顺序。
    // 没有 chunk 17（v9 及更早）这里会退化成纯白 —— 那正是 S3 要补的洞。
    out.slots.assign(slot_count, SpineGpuSlotSample {});
    for (uint32_t i = 0; i < slot_count; ++i)
    {
        const SpineGpuSlot& s = asset.slots[i];
        out.slots[i].color[0] = s.color_r;
        out.slots[i].color[1] = s.color_g;
        out.slots[i].color[2] = s.color_b;
        out.slots[i].color[3] = s.color_a;
        out.slots[i].dark[0] = s.dark_r;
        out.slots[i].dark[1] = s.dark_g;
        out.slots[i].dark[2] = s.dark_b;
        out.slots[i].dark[3] = s.dark_a;
        out.slots[i].attachment_name_offset = s.setup_attachment_name_offset;
    }
    out.draw_order.resize(slot_count);
    for (uint32_t i = 0; i < slot_count; ++i)
    {
        out.draw_order[i] = i;
    }

    const std::vector<uint8_t>& payload = asset.timeline_payload;

    for (uint32_t ti = 0; ti < anim.timeline_count; ++ti)
    {
        const uint32_t tl_index = anim.timeline_first + ti;
        if (tl_index >= asset.timelines.size())
        {
            return Fail("sample: 动画 " + std::to_string(anim_index) + " 引用了越界的时间轴 " +
                        std::to_string(tl_index));
        }
        const SpineGpuTimeline& tl = asset.timelines[tl_index];
        const SpineTimelineType type = static_cast<SpineTimelineType>(tl.type);
        const uint32_t entries = SpineTimelineEntries(type);
        if (entries == 0 || tl.frame_count == 0)
        {
            return Fail("sample: 第 " + std::to_string(tl_index) + " 条时间轴类型/帧数不自洽");
        }
        const uint64_t frame_end_u64 =
            static_cast<uint64_t>(tl.frame_first) + static_cast<uint64_t>(tl.frame_count) * entries;
        if (frame_end_u64 > asset.frames.size())
        {
            return Fail("sample: 第 " + std::to_string(tl_index) + " 条时间轴的帧区间越界");
        }
        const float* frames = asset.frames.data() + tl.frame_first;

        // ---- 公共的三段判定（四类都要，抄各自的 apply）----
        // 1) time < 第一帧 ⇒ **保持 setup**（alpha==1 下 MixBlend_First 是恒等变形）
        // 2) 只有一帧 或 time >= 最后一帧 ⇒ 取末帧
        // 3) 否则 ⇒ binarySearch 定位下一帧，插值
        const bool before_first = time < frames[0];

        if (type == SpineTimelineType::kColor || type == SpineTimelineType::kTwoColor)
        {
            if (tl.target >= slot_count)
            {
                return Fail("sample: 第 " + std::to_string(tl_index) + " 条颜色时间轴的槽位下标 " +
                            std::to_string(tl.target) + " 越界（只有 " + std::to_string(slot_count) +
                            " 个槽位）");
            }
            if (before_first)
            {
                continue;
            }
            SpineGpuSlotSample& dst = out.slots[tl.target];

            const bool two = (type == SpineTimelineType::kTwoColor);
            float r;
            float g;
            float b;
            float a;
            float r2 = 0.0f;
            float g2 = 0.0f;
            float b2 = 0.0f;

            const uint32_t last_frame = tl.frame_count - 1u;
            const float last_time = frames[static_cast<size_t>(last_frame) * entries];
            if (tl.frame_count == 1u || time >= last_time)
            {
                // "Time is after last frame." —— 取末帧的 PREV_*（ColorTimeline.cpp:83-88）
                const size_t base = static_cast<size_t>(last_frame) * entries;
                r = frames[base + 1];
                g = frames[base + 2];
                b = frames[base + 3];
                a = frames[base + 4];
                if (two)
                {
                    r2 = frames[base + 5];
                    g2 = frames[base + 6];
                    b2 = frames[base + 7];
                }
            }
            else
            {
                const uint32_t cur = SpineGpuBinarySearchFrames(frames, tl.frame_count, entries, time);
                const size_t prev = static_cast<size_t>(cur) - entries;
                float percent = 0.0f;
                {
                    const SpineImportResult cr = SpineGpuCurvePercentAt(asset, tl, tl_index, cur, time, percent);
                    if (!cr.ok)
                    {
                        return cr;
                    }
                }

                r = frames[prev + 1] + (frames[cur + 1] - frames[prev + 1]) * percent;
                g = frames[prev + 2] + (frames[cur + 2] - frames[prev + 2]) * percent;
                b = frames[prev + 3] + (frames[cur + 3] - frames[prev + 3]) * percent;
                a = frames[prev + 4] + (frames[cur + 4] - frames[prev + 4]) * percent;
                if (two)
                {
                    r2 = frames[prev + 5] + (frames[cur + 5] - frames[prev + 5]) * percent;
                    g2 = frames[prev + 6] + (frames[cur + 6] - frames[prev + 6]) * percent;
                    b2 = frames[prev + 7] + (frames[cur + 7] - frames[prev + 7]) * percent;
                }
            }

            dst.color[0] = r;
            dst.color[1] = g;
            dst.color[2] = b;
            dst.color[3] = a;
            if (two)
            {
                // ⚠️ dark 的 alpha **写死 1**（TwoColorTimeline.cpp:139）——
                //    容器里那个 dark_a 只表示 setup 暗色，不是"这一帧的暗色 alpha"。
                dst.dark[0] = r2;
                dst.dark[1] = g2;
                dst.dark[2] = b2;
                dst.dark[3] = 1.0f;
            }
            continue;
        }

        if (type == SpineTimelineType::kAttachment)
        {
            if (tl.target >= slot_count)
            {
                return Fail("sample: 第 " + std::to_string(tl_index) + " 条 attachment 时间轴的槽位下标 " +
                            std::to_string(tl.target) + " 越界");
            }
            if (before_first)
            {
                continue;
            }
            uint32_t frame_index = tl.frame_count - 1u;
            const float last_time = frames[tl.frame_count - 1u];
            if (tl.frame_count != 1u && time < last_time)
            {
                // `binarySearch(frames, time, 1) - 1`（AttachmentTimeline.cpp:74）
                frame_index = SpineGpuBinarySearchFrames(frames, tl.frame_count, 1u, time) - 1u;
            }
            if (frame_index >= tl.payload_count)
            {
                return Fail("sample: 第 " + std::to_string(tl_index) + " 条 attachment 时间轴的载荷帧数不够");
            }
            const size_t pos = static_cast<size_t>(tl.payload_offset) + static_cast<size_t>(frame_index) * 4u;
            if (pos + 4u > payload.size())
            {
                return Fail("sample: 第 " + std::to_string(tl_index) + " 条 attachment 时间轴的载荷越界");
            }
            // `kNoAttachmentName`（0xFFFFFFFF）= 空名 ⇒ 这一帧**不挂**附件
            // （AttachmentTimeline.cpp:79 `attachmentName->length() == 0 ? NULL : ...`）。
            out.slots[tl.target].attachment_name_offset = GetU32At(payload, pos);
            continue;
        }

        if (type == SpineTimelineType::kDrawOrder)
        {
            if (before_first)
            {
                continue;
            }
            uint32_t frame_index = tl.frame_count - 1u;
            const float last_time = frames[tl.frame_count - 1u];
            if (tl.frame_count != 1u && time < last_time)
            {
                frame_index = SpineGpuBinarySearchFrames(frames, tl.frame_count, 1u, time) - 1u;
            }

            // 载荷是**变长**的（每帧 n 不同）⇒ 必须从头走到第 frame_index 帧。
            size_t pos = tl.payload_offset;
            uint32_t n = 0;
            for (uint32_t fr = 0; fr <= frame_index; ++fr)
            {
                if (pos + 4u > payload.size())
                {
                    return Fail("sample: 第 " + std::to_string(tl_index) + " 条 drawOrder 时间轴的载荷被截断");
                }
                n = GetU32At(payload, pos);
                pos += 4u;
                if (n != 0 && fr == frame_index)
                {
                    break;   // 最后一帧：读出 n 就够了，下面再读它的 (slot, offset) 对
                }
                if (pos + static_cast<size_t>(n) * 8u > payload.size())
                {
                    return Fail("sample: 第 " + std::to_string(tl_index) + " 条 drawOrder 时间轴的载荷越界");
                }
                pos += static_cast<size_t>(n) * 8u;
            }
            if (n == 0)
            {
                // `_drawOrders[frame].size() == 0` ⇒ 复位成 setup 顺序
                // （DrawOrderTimeline.cpp:87-92）。跑到这里时它本来就是 setup 序。
                continue;
            }
            if (pos + static_cast<size_t>(n) * 8u > payload.size())
            {
                return Fail("sample: 第 " + std::to_string(tl_index) + " 条 drawOrder 时间轴的载荷越界");
            }

            // ---- 重建完整排列（SkeletonBinary.cpp:1040-1072 / SkeletonJson.cpp:1129-1165）----
            // ⚠️ **不是**"把槽位挪到 index+offset 上"。参考实现的做法有三处不对称，
            //    每一处抄错都能得到一个看起来合理的排列：
            //   a) `drawOrder[originalIndex + offset] = originalIndex` —— 下标用的是
            //      **原始（setup）下标**，而且 offset 是**有符号**的；
            //   b) 未列出的槽位收进 `unchanged`，**按槽位升序**收集；
            //   c) 回填时从**数组末尾往前**填，`unchanged` 也从**末尾往前**取 ——
            //      所以剩下的空洞里填的不是"剩余槽位的升序"。
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
                    return Fail("sample: 第 " + std::to_string(tl_index) +
                                " 条 drawOrder 时间轴的槽位下标不是升序（参考实现在这里是死循环）");
                }
                while (original_index != slot_index)
                {
                    unchanged.push_back(static_cast<int32_t>(original_index));
                    ++original_index;
                }
                const int64_t target = static_cast<int64_t>(original_index) + offset;
                if (target < 0 || target >= static_cast<int64_t>(slot_count))
                {
                    return Fail("sample: 第 " + std::to_string(tl_index) + " 条 drawOrder 时间轴把槽位 " +
                                std::to_string(original_index) + " 挪到了越界位置 " + std::to_string(target));
                }
                order[static_cast<size_t>(target)] = static_cast<int32_t>(original_index);
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
                        return Fail("sample: 第 " + std::to_string(tl_index) +
                                    " 条 drawOrder 时间轴的排列重建失败（空洞比未列出槽位多）");
                    }
                    order[static_cast<size_t>(ii)] = unchanged[--ui];
                }
            }
            for (uint32_t i = 0; i < slot_count; ++i)
            {
                out.draw_order[i] = static_cast<uint32_t>(order[i]);
            }
            continue;
        }

        // rotate / translate / scale / shear / deform / 约束 / event：**不是 S3 的范围**。
        // 明确跳过而不是"顺手算" —— 骨骼要等约束与 update 顺序（S4），deform 要等
        // 共享 VB 上的增量方案（S6）。半实现的解算器比没有更危险：它会让"还没做"
        // 的部分看起来已经能跑。
    }

    SpineImportResult r;
    r.ok = true;
    return r;
}
