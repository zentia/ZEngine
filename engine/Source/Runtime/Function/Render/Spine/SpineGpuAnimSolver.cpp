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
//   * 各时间轴的 apply（含 alpha / blend / direction）S5 起在 SpineGpuSkeletonState.cpp
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
    const size_t ci = static_cast<size_t>(tl.curve_first) + segment;
    if (ci >= asset.curve_types.size())
    {
        return Fail("sample: 第 " + std::to_string(tl_index) + " 条时间轴的曲线段下标越界");
    }
    const SpineCurveType curve = static_cast<SpineCurveType>(asset.curve_types[ci]);
    const float* bez_table = nullptr;
    if (curve == SpineCurveType::kBezier)
    {
        // ⚠️ `curve_bezier_index` 是**派生**数据（加载后由 `SpineGpuBuildCurveCache()` 建）。
        //    没建就在这里报错，绝不退回"从段 0 扫一遍数 bezier"—— 那只慢不报错，
        //    而"慢"正是这类 bug 唯一的表现。
        if (asset.curve_bezier_index.size() != asset.curve_types.size())
        {
            return Fail("sample: 第 " + std::to_string(tl_index) +
                        " 条时间轴要读 bezier，但曲线缓存没建（烘焙 / 读容器后必须调 "
                        "SpineGpuBuildCurveCache）—— 不在这里悄悄退化成 O(段数) 的慢路径");
        }
        const uint32_t bi = asset.curve_bezier_index[ci];
        if (bi == kSpineNoBezierIndex)
        {
            return Fail("sample: 第 " + std::to_string(tl_index) + " 条时间轴第 " + std::to_string(segment) +
                        " 段标了 bezier 但缓存里没有控制点");
        }
        if (static_cast<size_t>(bi) * kSpineBezierTableStride + kSpineBezierTableStride >
            asset.bezier_tables.size())
        {
            return Fail("sample: 第 " + std::to_string(tl_index) + " 条时间轴的 bezier 表下标越界");
        }
        bez_table = &asset.bezier_tables[static_cast<size_t>(bi) * kSpineBezierTableStride];
    }
    percent_out = SpineGpuCurvePercent(curve, bez_table, percent);
    return Ok();
}

// 定义在下面（`SpineGpuCurvePercent` 旁边）：把 4 个控制点展开成 18 个采样点。
void BuildBezierTable(const SpineGpuBezier& b, float* out);

SpineImportResult SpineGpuBuildCurveCache(SpineGpuAsset& asset)
{
    asset.curve_bezier_index.assign(asset.curve_types.size(), kSpineNoBezierIndex);
    asset.bezier_tables.assign(static_cast<size_t>(asset.beziers.size()) * kSpineBezierTableStride, 0.0f);

    // 每条 bezier 展开一次（18 float：9 个采样点的 x,y 交错，与参考 `_curves` 同布局）。
    for (size_t i = 0; i < asset.beziers.size(); ++i)
    {
        BuildBezierTable(asset.beziers[i], &asset.bezier_tables[i * kSpineBezierTableStride]);
    }

    // 每段 bezier 的表条号：时间轴 × 段序扫一遍（**只在加载时**），之后求值 O(1)。
    for (uint32_t tli = 0; tli < asset.timelines.size(); ++tli)
    {
        const SpineGpuTimeline& tl = asset.timelines[tli];
        if (!SpineTimelineHasCurve(static_cast<SpineTimelineType>(tl.type)))
        {
            continue;   // attachment / drawOrder 没有曲线
        }
        if (tl.frame_count < 2u)
        {
            continue;   // 段数 = 帧数 - 1 ⇒ 0 段
        }
        const uint32_t segments = tl.frame_count - 1u;
        if (static_cast<size_t>(tl.curve_first) + segments > asset.curve_types.size())
        {
            return Fail("curve cache: 第 " + std::to_string(tli) + " 条时间轴的曲线段区间越界");
        }
        uint32_t nth = 0;
        for (uint32_t s = 0; s < segments; ++s)
        {
            const size_t ci = static_cast<size_t>(tl.curve_first) + s;
            if (asset.curve_types[ci] != static_cast<uint8_t>(SpineCurveType::kBezier))
            {
                continue;
            }
            if (nth >= tl.bezier_count)
            {
                return Fail("curve cache: 第 " + std::to_string(tli) + " 条时间轴有 " +
                            std::to_string(tl.bezier_count) + " 条 bezier，但第 " + std::to_string(s) +
                            " 段又标了一条");
            }
            const uint32_t bi = tl.bezier_first + nth;
            if (bi >= asset.beziers.size())
            {
                return Fail("curve cache: 第 " + std::to_string(tli) + " 条时间轴的 bezier 下标越界");
            }
            asset.curve_bezier_index[ci] = bi;
            ++nth;
        }
        if (nth != tl.bezier_count)
        {
            return Fail("curve cache: 第 " + std::to_string(tli) + " 条时间轴标了 " +
                        std::to_string(tl.bezier_count) + " 条 bezier，但只有 " + std::to_string(nth) +
                        " 段曲线是 bezier（容器不自洽）");
        }
    }
    return Ok();
}

// bezier 查找表的展开：`CurveTimeline::setCurve()`（:78-105）。
// 输出 9 个采样点（t = 0.1 .. 0.9），写成 **x,y 交错**的 18 个 float —— 与参考实现的
// `_curves` 同一布局（`BEZIER_VALUES = 10*2-2 = 18`，`_curves[i] = x, _curves[i+1] = y`）。
//
// ⚠️ 4 个常数 `0.03f / 0.006f / 0.3f / 0.16666667f` **照抄**：
//    它们是"步长 0.1 的三次 bezier 前向差分"的系数，不是可以重排的算术式，
//    化简即错且不报错（曲线会悄悄变成另一条）。
//
// `out` 至少 18 个 float（= `kSpineBezierTableStride`）。
void BuildBezierTable(const SpineGpuBezier& b, float* out)
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
        out[n * 2] = x;
        out[n * 2 + 1] = y;
        dfx += ddfx;
        dfy += ddfy;
        ddfx += dddfx;
        ddfy += dddfy;
        x += dfx;
        y += dfy;
    }
}

float SpineGpuCurvePercent(SpineCurveType type, const float* bezier_table, float percent)
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
    if (bezier_table == nullptr)
    {
        return percent;   // 容器不自洽（标了 bezier 却没有表）—— 读端 / 采样已经报过了
    }

    // `getCurvePercent()`（:118-141）：找第一个 x >= percent 的点，与前一点线性插值。
    // 表是 x,y 交错的 18 个 float（与参考的 `_curves` 同布局）。
    float x = 0.0f;
    for (int n = 0; n < 9; ++n)
    {
        x = bezier_table[n * 2];
        if (x >= percent)
        {
            const float prev_x = (n == 0) ? 0.0f : bezier_table[(n - 1) * 2];
            const float prev_y = (n == 0) ? 0.0f : bezier_table[(n - 1) * 2 + 1];
            return prev_y + (bezier_table[n * 2 + 1] - prev_y) * (percent - prev_x) / (x - prev_x);
        }
    }
    // 落在最后一个采样点之后：与 (1,1) 连线。
    const float last_y = bezier_table[8 * 2 + 1];
    return last_y + (1.0f - last_y) * (percent - x) / (1.0f - x);
}

std::vector<uint8_t> BuildSpineStringBlob(const SpineGpuAsset& asset)
{
    std::vector<uint8_t> blob;
    std::vector<uint32_t> slot_off;
    std::vector<uint32_t> anim_off;
    std::vector<uint32_t> anim_att_off;
    std::vector<uint32_t> path_att_off;
    std::vector<uint32_t> clip_off;
    BuildSpineStringTable(asset, blob, slot_off, anim_off, anim_att_off, path_att_off, clip_off);
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

// `SpineGpuSampleSlots()` 的实现在 SpineGpuPoseSolver.cpp（S5 起槽位侧与骨骼侧共用同一套
// 时间轴 apply，见 SpineGpuSkeletonState.h）。
