#pragma once

// =====================================================================
// GPU-Spine 动画解算（槽位侧）：容器 → 某一时刻的槽位状态
// 规格：doc/GPU_SPINE_V7_ANIMATION.md §3.2 / §3.5
// ---------------------------------------------------------------------
// 本文件只做 **S3 的四类时间轴**：color / twoColor / attachment / drawOrder。
// 骨骼（rotate / translate / scale / shear）是 S4 —— 它依赖约束与 update 顺序；
// deform 是 S6 —— 它依赖共享 VB 与每实例增量。这里遇到它们就**跳过**，
// 不是为了省事，是因为"跳过"和"算错"在测试里的表现完全一样：
// 只有把范围写清楚，才能让"没实现"和"实现错了"区分开。
//
// 语义口径（照抄参考实现，不自行发挥）：
//   `AnimationState::apply()` 对**轨道 0 上唯一一条、已淡入完的**动画用的是
//     `animation->apply(skeleton, lastTime, time, loop, events, 1.0f, MixBlend_First, MixDirection_In)`
//   （`AnimationState.cpp:388` `blend = i == 0 ? MixBlend_First : ...`、
//    `:402` `if ((i == 0 && mix == 1) || ...)`）。
//   alpha == 1 时 MixBlend_First 与"直接写"等价（`ColorTimeline.cpp:107-112`），
//   而 `time < frames[0]` 那一路在 alpha==1 下是恒等变形（`color += (color - setup) * 1`
//   当 color == setup 时不变）⇒ **本解算器 = 先全部复位到 setup，再按时间轴覆盖**。
//
// ⚠️ 每一条公式都带参考实现的出处（G:/OSGame/unity-engine-source/Modules/Spine/spine-cpp），
//    不要凭印象改。
// =====================================================================

#include "Runtime/Function/Render/Spine/SpineGpuFormat.h"
#include "Runtime/Function/Render/Spine/SpineGpuImporter.h"

// 曲线求值：把"段内的线性进度" percent ∈ [0,1] 映射成"插值用的" percent。
// `bezier` 只在 type == kBezier 时被读（其它类型传 nullptr 也安全）。
//
// 抄 `CurveTimeline::getCurvePercent()`（CurveTimeline.cpp:110-141）：
//   * LINEAR  → 原样返回
//   * STEPPED → 恒返回 **0**（阶梯 = 一直停在前一帧）
//   * BEZIER  → 在参考实现那张 18 值的查找表里找第一个 x >= percent 的点，线性插值
//
// ⚠️ 容器里存的是 **4 个控制点**（16 B），不是那张表
//    （定长存表要 49.0 MB，存控制点 16.8 MB，实测差 65%）。
//    ⇒ 这张表每次求值都要**重建**（18 个 float，纯前向差分，见下）。
//    重建的系数 `0.03f / 0.006f / 0.3f / 0.16666667f`（CurveTimeline.cpp:78-81）
//    是 bezier 的前向差分系数，**照抄，化简即错且不报错**。
float SpineGpuCurvePercent(SpineCurveType type, const SpineGpuBezier* bezier, float percent);

// 一个槽位在某一时刻的解算结果。
struct SpineGpuSlotSample
{
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float dark[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    // 当前挂的附件名在 chunk 1 里的字节偏移；`kNoAttachmentName` = 这一帧**不挂**附件。
    uint32_t attachment_name_offset = SpineGpuConstants::kNoAttachmentName;
};

struct SpineGpuSlotSampleResult
{
    // 下标 = header.slot_count 的序 = chunk 17 的行号。
    std::vector<SpineGpuSlotSample> slots;
    // 绘制顺序：槽位下标的一个排列。没有 drawOrder 时间轴命中时 = 0,1,2,...（setup 序）。
    std::vector<uint32_t> draw_order;
};

// 采样 `asset` 的第 `anim_index` 个动画在 `time` 处的**槽位状态**。
//
// ⚠️ `time` 不在这里取模 —— 循环是调用方的事（参考实现也是 `Animation::apply`
//    只在 `loop` 为真时才 `fmod`，且 `AnimationState` 自己管 trackTime）。
//
// 出错（返回 !ok）只可能是"容器不自洽"，而这类不自洽**不会崩溃**：
// 越界读在 C++ 里不报错、在 C# 里也不抛，只会拿到别人的数据 ⇒ 全部显式查。
SpineImportResult SpineGpuSampleSlots(const SpineGpuAsset& asset, uint32_t anim_index, float time,
                                      SpineGpuSlotSampleResult& out);

// 按落盘顺序把 asset 的字符串表拼回一整块（chunk 1 的字节镜像）。
// 有了它才能把"名字偏移"翻译回字符串 —— 容器里只有偏移，且偏移是全表唯一的。
std::vector<uint8_t> BuildSpineStringBlob(const SpineGpuAsset& asset);

// 在 blob 里取 offset 处的 C 串。越界（offset >= blob.size()）返回 nullptr。
const char* SpineGpuStringAt(const std::vector<uint8_t>& blob, uint32_t offset);
