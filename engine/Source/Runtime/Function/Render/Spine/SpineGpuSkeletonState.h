#pragma once

// =====================================================================
// GPU-Spine S5：**可变的**骨架状态 + 时间轴 apply（alpha / MixBlend / MixDirection）
// 规格：doc/GPU_SPINE_V7_ANIMATION.md §7.0.8
// ---------------------------------------------------------------------
// S2-S4 的解算器是**无状态**的："先复位到 setup，再用 alpha=1 / MixBlend_First 覆盖"。
// 这只对"轨道 0 上唯一一条、已淡入完的动画"成立。多轨混合（AnimationState）的语义
// 依赖**上一帧留下来的值**：
//   * alpha < 1 的 First / Replace 是 `bone._x += (目标 - bone._x) * alpha` —— 从**当前值**出发；
//   * 没有任何时间轴命中的属性**保持原值**（不复位）；
//   * mesh deform 数组挂在**槽位**上，换附件才清空（Slot::setAttachment，Slot.cpp:94-102）；
//   * transform 约束的 absolute+local 分支会写**局部** shearY，并一直留到下一帧
//     （SpineGpuPoseSolver.cpp `TransformApplyAbsoluteLocal` 的注释）。
// ⇒ 必须有一份跨帧存活的状态，对应参考实现里的 `spine::Skeleton`。
//
// 本文件只做"时间轴 → 状态"这一半；"状态 → 世界变换"（约束 + 遍历）在
// `SpineGpuUpdateWorld`（SpineGpuPoseSolver.h），多轨调度在 `SpineGpuAnimationState`。
//
// ⚠️ 每个 apply 分支都**逐字照抄**参考实现（含它的笔误），出处写在 .cpp 里。
//    已知照抄的参考实现行为（改了就与参考对不上，且不报错）：
//      1. `TwoColorTimeline` 在 `time < 第 0 帧` 且 MixBlend_First 时，写的是
//         `color += (color - setup) * alpha`（符号反了，TwoColorTimeline.cpp:84-93）；
//      2. `Slot::setToSetupPose()` **不复位暗色**（Slot.cpp:54-64，只复位亮色与附件），
//         暗色初值是构造函数里的 (0,0,0,0) ⇒ 只有 TwoColor 时间轴会改它；
//      3. 颜色 `set` / `add` 会夹到 [0,1]（Color.h），但 TwoColor 的"第 0 帧之前"分支
//         是直接 `+=`，**不夹**。
//
// 参考实现：G:/OSGame/unity-engine-source/Modules/Spine/spine-cpp（3.8）
// =====================================================================

#include "Runtime/Function/Render/Spine/SpineGpuAnimSolver.h"

#include <cstdint>
#include <string>
#include <vector>

// `MixBlend.h` 原值（不自造：AnimationState 里 `i == 0 ? MixBlend_First : ...` 这类比较照搬）
enum class SpineGpuMixBlend : uint8_t
{
    kSetup = 0,
    kFirst = 1,
    kReplace = 2,
    kAdd = 3,
};

// `MixDirection.h` 原值
enum class SpineGpuMixDirection : uint8_t
{
    kIn = 0,
    kOut = 1,
};

// `r -= (16384 - (int)(16384.499999999996 - r / 360)) * 360` —— 把角度收进 (-180, 180]。
//
// ⚠️ 它**不是** `fmod`：`(16384 - (int)(...)) * 360` 全程是 **int** 运算，最后才转 float；
//    `(int)` 是**向零截断**，不是 floor。改写成 fmod 会在负数侧差 360°，且不报错。
//    参考实现里它散落在 RotateTimeline / AnimationState::applyRotateTimeline /
//    TransformConstraint 三处，全部走这一份。
inline float SpineGpuWrapDegrees(float r)
{
    const int k = static_cast<int>(16384.499999999996 - static_cast<double>(r / 360.0f));
    return r - static_cast<float>((16384 - k) * 360);
}

// spine-cpp 的 `Bone` 的全部可变状态。分三层，与参考实现**逐字段对应**，不要合并：
//   * `x..shear_y`   = 局部值（时间轴写它）
//   * `ax..ashear_y` = "applied" 值（约束读/写它；`updateWorldTransform` 也会同步它）
//   * `a..world_y`   = 世界变换（最终要上传的东西）
// 初值照 `Bone::Bone()`（Bone.cpp:53-78）：applied 全 0（**包括 scale**）、世界矩阵单位阵。
struct SpineGpuBoneState
{
    // 局部
    float x = 0.0f;
    float y = 0.0f;
    float rotation = 0.0f;
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    float shear_x = 0.0f;
    float shear_y = 0.0f;
    // applied
    float ax = 0.0f;
    float ay = 0.0f;
    float arotation = 0.0f;
    float ascale_x = 0.0f;
    float ascale_y = 0.0f;
    float ashear_x = 0.0f;
    float ashear_y = 0.0f;
    // 世界
    float a = 1.0f;
    float b = 0.0f;
    float c = 0.0f;
    float d = 1.0f;
    float world_x = 0.0f;
    float world_y = 0.0f;

    bool applied_valid = false;

    // setup 常量（从 chunk 10 抄过来，只为了解算时少一次间接寻址）
    float length = 0.0f;
    int32_t parent = -1;
    uint32_t mode = 0;
};

// 槽位当前挂的附件。参考实现里是一根 `Attachment*`，我们没有附件对象 ⇒ 用"名字"做身份：
// `Skeleton::getAttachment(slot, name)` 在同一槽位、同一皮肤下按名字唯一确定一个附件，
// 所以"同名 ⇔ 同一附件"（`Slot::setAttachment` 的 `_attachment == inValue` 判据）。
//
// ⚠️ 唯一的差异：名字在皮肤里**找不到**时参考实现得到 NULL，两个都找不到的名字在参考里
//    是"同一个 NULL"，在这里是两个不同的名字 ⇒ 切换时多清一次 deform 数组。这在 NULL
//    附件上不可观察（任何可画附件挂上去时都会清），所以不为它多存一份"是否找得到"。
struct SpineGpuAttachRef
{
    static constexpr uint32_t kNone = 0xFFFFFFFFu;
    uint32_t name_id = kNone;   // 规范化后的名字 id（`SpineGpuRuntimeData::names` 的下标）；kNone = 不挂
    uint32_t name_offset = SpineGpuConstants::kNoAttachmentName;   // chunk 1 偏移（输出用）
    uint32_t draw_item = kNone;   // default 皮肤（skin 0）下对应的绘制项；kNone = 不产生绘制项
    uint32_t path_att = SpineGpuConstants::kNoPathAttachment;   // chunk 20 下标；不是 path = kNoPathAttachment
};

// 从容器派生出来、**不随帧变**的查找表。每个资产建一次，所有实例 / 所有帧共用。
//
// 以前每次采样都重建字符串表并按名字线性查绘制项（`BuildSpineStringBlob` + 字符串比较），
// 多轨每帧要查好几遍 ⇒ 全部前移到这里。
struct SpineGpuRuntimeData
{
    std::vector<uint8_t> blob;                // chunk 1 的字节镜像
    std::vector<std::string> names;           // 规范化后的附件名（name_id 的下标空间）

    std::vector<SpineGpuAttachRef> slot_setup;   // [slot] setup 附件

    // attachment 时间轴：第 tl 条的第 f 帧 = attach_refs[timeline_ref_first[tl] + f]
    std::vector<uint32_t> timeline_ref_first;   // [timeline]，非 attachment 时间轴 = 0（不读）
    std::vector<SpineGpuAttachRef> attach_refs;

    // drawOrder 时间轴：第 tl 条的第 f 帧的**完整排列**（参考实现读文件时就拼好的
    // `_drawOrders[f]`，SkeletonBinary.cpp:1040-1072）。空帧（= 复位成 setup 序）记为空。
    // 第 tl 条的第 f 帧 = draw_order_perm[draw_order_first[tl] + f]（偏移进 draw_order_pool）
    std::vector<uint32_t> draw_order_first;      // [timeline]
    std::vector<uint32_t> draw_order_frame_pos;  // [Σ帧] 进 draw_order_pool 的下标；kNone = 空帧
    std::vector<uint32_t> draw_order_pool;       // 排列拼接，每段 slot_count 个

    // deform 时间轴：第 tl 条的第 f 帧载荷 (start, count, values*) 的字节位置
    // = deform_frame_pos[deform_frame_first[tl] + f]（载荷变长，不建表就只能每次从头走）
    std::vector<uint32_t> deform_frame_first;    // [timeline]
    std::vector<uint32_t> deform_frame_pos;

    // 绘制项的 deform 长度（rigid 2 × 顶点数、weighted 2 × 影响数，= `DeformTimeline::_deformLength`）
    std::vector<uint32_t> item_deform_len;       // [draw item]

    // 时间轴的"属性身份"（参考实现的 `Timeline::getPropertyId()`），AnimationState 拿它判
    // "两条时间轴写的是不是同一个属性"。
    //
    // ⚠️ 不照抄参考实现的整数编码：`DeformTimeline::getPropertyId()` 是
    //    `(Deform << 24) + attachment->_id + slotIndex`（DeformTimeline.cpp:264），而
    //    `_id = (全局计数器++ & 65535) << 11`（VertexAttachment.cpp:158-161）——
    //    计数器是**进程级**的、随加载历史变，算出来的值会越界撞进别的类型的号段
    //    （例：计数器 = 16384、槽位 0 时恰好等于 DrawOrder 的 `8 << 24`）。
    //    那是参考实现的偶发撞号，不是语义 ⇒ 这里用 (类型, 目标) 的无碰撞编码，
    //    对拍台单独统计参考侧的撞号（见 SpineGpuRefParityTest 的 S5 段）。
    std::vector<uint64_t> property_key;          // [timeline]
};

struct SpineGpuSlotState
{
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float dark[4] = {0.0f, 0.0f, 0.0f, 0.0f};   // 见文件头第 2 条：不随 setup 复位
    SpineGpuAttachRef attachment;
    // `Slot::_attachmentVertices`：rigid = 绝对局部坐标、weighted = 逐影响偏移；空 = 用 setup。
    std::vector<float> deform;
};

// 对应参考实现的 `spine::Skeleton`（只含会被时间轴 / 约束改写的部分）。
// IK / transform 约束的运行时参数没有放进来：它们只能被 IK / transform 时间轴改，
// 而这两类时间轴本阶段**显式报错**（实测 0 / 782 个资产用到）⇒ 恒等于 setup。
struct SpineGpuSkeletonState
{
    std::vector<SpineGpuBoneState> bones;        // [bone]
    std::vector<SpineGpuSlotState> slots;        // [slot]
    std::vector<uint32_t> draw_order;            // 槽位下标的排列
    // path 约束的运行时参数（`PathConstraint::_position` 等）
    std::vector<float> path_position;
    std::vector<float> path_spacing;
    std::vector<float> path_rotate_mix;
    std::vector<float> path_translate_mix;
};

// 建查找表。出错 = 容器不自洽（读端应该已经报过，这里再挡一次：越界读不报错）。
SpineImportResult SpineGpuBuildRuntimeData(const SpineGpuAsset& asset, SpineGpuRuntimeData& out);

// `Skeleton::Skeleton()`：骨骼 / 槽位按各自构造函数的初值，再各自 setToSetupPose 一次。
SpineImportResult SpineGpuInitSkeletonState(const SpineGpuAsset& asset, const SpineGpuRuntimeData& data,
                                            SpineGpuSkeletonState& out);

// `Skeleton::setBonesToSetupPose()`（Skeleton.cpp:223-259）：骨骼**局部**值 + 约束运行时参数。
void SpineGpuSetBonesToSetupPose(const SpineGpuAsset& asset, SpineGpuSkeletonState& state);
// `Skeleton::setSlotsToSetupPose()`（Skeleton.cpp:261-270）：绘制顺序 + 每个槽位的
// 颜色 / 附件（附件**强制**重挂 ⇒ deform 必清，Slot.cpp:58-60；暗色不动，见文件头）。
void SpineGpuSetSlotsToSetupPose(const SpineGpuAsset& asset, const SpineGpuRuntimeData& data,
                                 SpineGpuSkeletonState& state);
void SpineGpuSetToSetupPose(const SpineGpuAsset& asset, const SpineGpuRuntimeData& data, SpineGpuSkeletonState& state);

// `Slot::setAttachment()`（Slot.cpp:94-102）：同一附件 ⇒ 什么都不做；否则换上并清 deform。
void SpineGpuSetSlotAttachment(SpineGpuSlotState& slot, const SpineGpuAttachRef& ref);

// `Timeline::apply(skeleton, lastTime, time, events, alpha, blend, direction)`。
// event 时间轴不改状态（这里不收集事件）；IK / transform / pathSpacing 时间轴**显式报错**。
SpineImportResult SpineGpuApplyTimeline(const SpineGpuAsset& asset, const SpineGpuRuntimeData& data,
                                        SpineGpuSkeletonState& state, uint32_t tl_index, float last_time, float time,
                                        float alpha, SpineGpuMixBlend blend, SpineGpuMixDirection direction);

// `Animation::apply(skeleton, lastTime, time, loop, events, alpha, blend, direction)`（Animation.cpp:54-66）：
// loop 且时长非 0 时先对 time（与 lastTime > 0 时的 lastTime）取模，再按容器顺序逐条 apply。
//
// 容器里的时间轴按 (type, target) 排序，参考实现按文件书写顺序（槽位 → 骨骼 → 约束 → deform →
// drawOrder → event）。唯一有先后依赖的是 attachment → deform（换附件会清 deform 数组），
// 而 type 值 Attachment = 4 < Deform = 6 ⇒ 排序后的顺序同样满足，不需要重排。
SpineImportResult SpineGpuApplyAnimation(const SpineGpuAsset& asset, const SpineGpuRuntimeData& data,
                                         SpineGpuSkeletonState& state, uint32_t anim_index, float last_time,
                                         float time, bool loop, float alpha, SpineGpuMixBlend blend,
                                         SpineGpuMixDirection direction);

// `AnimationState::applyRotateTimeline()`（AnimationState.cpp:636-715）：alpha < 1 时
// 按"首帧定方向、之后检测跨零"的最短弧混合。`timelines_rotation` 指向该时间轴的那 **2** 个
// float（`_timelinesRotation[i]` 与 `[i + 1]`）。放在这里而不是 AnimationState 里：它要读
// 帧与曲线，和 RotateTimeline::apply 共用同一套取帧代码才不会两边漂移。
SpineImportResult SpineGpuApplyRotateTimelineMixed(const SpineGpuAsset& asset, SpineGpuSkeletonState& state,
                                                   uint32_t tl_index, float time, float alpha, SpineGpuMixBlend blend,
                                                   float* timelines_rotation, bool first_frame);
