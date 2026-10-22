#pragma once

// =====================================================================
// GPU-Spine 动画解算（骨骼侧）：容器 → 某一时刻的**完整姿势**
// 规格：doc/GPU_SPINE_V7_ANIMATION.md §3.4 / §3.6 / §3.7、§4.1
// ---------------------------------------------------------------------
// 这个文件是 **S4b**：骨骼时间轴 + 三类约束求解 + 按 chunk 19 遍历。
// 槽位侧（color / twoColor / attachment / drawOrder）在 `SpineGpuAnimSolver`。
//
// 语义口径与槽位侧完全一致（照抄参考实现，不自行发挥）：
//   `AnimationState::apply()` 对**轨道 0 上唯一一条、已淡入完的**动画用的是
//     `animation->apply(skeleton, lastTime, time, loop, events, 1.0f, MixBlend_First, MixDirection_In)`
//   alpha == 1 的 MixBlend_First 等价于"**先复位到 setup，再让时间轴覆盖**"。
//
// ⚠️ 三条"改了不报错"的总纲（每一条都在 .cpp 里标了出处）：
//   1. **数学按 3.8**（§4.1）：3.7 与 3.8 有 2 处真差异，两处都是 3.8 修掉的 3.7 bug。
//      3.7 算出的是错姿势，且不报错。
//   2. **执行顺序落的是结果**（chunk 19），运行时照着遍历，不自己重排 ——
//      重排规则有 `sortBone` 递归 + 三类约束共用的 `order` 两层，写反只是姿势略偏。
//   3. **参考实现的"疑似笔误"也照抄**。已发现一例：
//      `TransformConstraint::applyAbsoluteLocal()` 的 shear 分支写的是 `bone._shearY`
//      （局部量）而不是局部变量 `shearY`，所以 **shearMix 在 absolute+local 模式下
//      对当帧世界变换没有作用**。别"顺手修"——修了就与参考实现对不上，且不报错。
//
// 参考实现：G:/OSGame/unity-engine-source/Modules/Spine/spine-cpp（3.8）
// =====================================================================

#include "Runtime/Function/Render/Spine/SpineGpuAnimSolver.h"

// 一根骨骼在某一时刻的解算结果：**世界** 2x2 + 平移。
// 这就是要上传给 GPU 的那个矩阵（`SpineBoneTransform`，§5.1）。
struct SpineGpuBoneSample
{
    float a = 1.0f;   // 世界矩阵 [a b]
    float b = 0.0f;   //          [c d]
    float c = 0.0f;
    float d = 1.0f;
    float world_x = 0.0f;
    float world_y = 0.0f;
};

// 某一时刻的完整姿势 = 骨骼世界变换 + 槽位状态 + 绘制顺序。
//
// 继承而不是并列：`SpineGpuSampleSlots()` 可以直接往里写槽位侧，
// 调用点不用 `res.slots.slots[i]` 这种绕的写法。
struct SpineGpuPoseResult : SpineGpuSlotSampleResult
{
    std::vector<SpineGpuBoneSample> bones;   // [header.bone_count]
};

// 采样 `asset` 的第 `anim_index` 个动画在 `time` 处的**完整姿势**。
//
// 顺序（与参考实现一致，不能调换）：
//   1. 骨骼复位到 setup（chunk 10）
//   2. 槽位侧采样（attachment 时间轴会改变 path 约束看到的那条曲线）
//   3. 骨骼时间轴 + 约束时间轴改写局部值 / 约束参数
//   4. 按 chunk 19 的复位表复位 `_updateCacheReset` 里的骨骼
//   5. 按 chunk 19 的步骤序列跑 `update()`
//
// ⚠️ `time` 不在这里取模 —— 循环是调用方的事（参考实现也是 `Animation::apply`
//    只在 `loop` 为真时才 `fmod`，且 `AnimationState` 自己管 trackTime）。
//
// 出错（返回 !ok）只可能是"容器不自洽"或"遇到了本阶段不做的时间轴"。
// 后者**显式报错**而不是跳过 —— 跳过会得到"看起来合理但错"的姿势。
// 诊断用：逐步记录解算过程（对拍台定位"第一个分叉的步骤"）。
// 生产代码传 nullptr，零开销。
struct SpineGpuPoseTrace
{
    struct Local
    {
        float x, y, rotation, scale_x, scale_y, shear_x, shear_y;
    };
    // 时间轴写完、复位表之前的**局部**值（= 参考实现 `Bone::_x` 等）
    std::vector<Local> local;
    // chunk 19 每一步之后的全体骨骼世界变换（与 `step_kind` / `step_index` 一一对应）
    std::vector<uint8_t> step_kind;     // SpineGpuUpdateKind
    std::vector<uint32_t> step_index;
    std::vector<std::vector<SpineGpuBoneSample>> after_step;
    // 同一时刻的 applied 值（ax, ay, arotation, ascale_x, ascale_y, ashear_x, ashear_y）
    // 与 applied_valid。世界矩阵对得上、applied 对不上时，下一个约束照样会解错。
    std::vector<std::vector<Local>> applied_after_step;
    std::vector<std::vector<uint8_t>> applied_valid_after_step;
};

SpineImportResult SpineGpuSamplePose(const SpineGpuAsset& asset, uint32_t anim_index, float time,
                                     SpineGpuPoseResult& out, SpineGpuPoseTrace* trace = nullptr);
