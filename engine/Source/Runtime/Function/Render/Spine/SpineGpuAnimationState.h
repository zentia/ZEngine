#pragma once

// =====================================================================
// GPU-Spine S5：多轨混合（`spine::AnimationState` 的转录）
// 规格：doc/GPU_SPINE_V7_ANIMATION.md §7.0.8
// ---------------------------------------------------------------------
// 逐函数转录 spine-cpp 3.8 的 AnimationState.cpp（1033 行），**不重新设计**：
//   update / apply / applyMixingFrom / updateMixingFrom / applyRotateTimeline /
//   setAnimation / addAnimation / setEmptyAnimation(s) / addEmptyAnimation /
//   clearTrack(s) / setCurrent / animationsChanged / setTimelineModes / hasTimeline
// 每个函数旁标了参考实现的行号；分支、浮点运算顺序、MathUtil::min/max 的写法都照抄。
//
// 刻意**不做**的（与姿势无关，不影响任何一个输出值）：
//   * 事件（start / interrupt / end / dispose / complete / event 回调）—— 只保留它们对状态的
//     副作用：start / end 置 `animationsChanged`，end / dispose 把 TrackEntry 还回对象池；
//   * `eventThreshold`（只决定事件收不收）。
//
// ⚠️ 照抄的参考实现行为（改了与参考对不上，且不报错）：
//   1. `setTimelineModes()` 的 HoldMix 分支写的是 `timelineHoldMix[i] = entry`（**自己**），
//      不是向后找到的那条 `next`（AnimationState.cpp:1013）。效果：HoldMix 的淡出速度按
//      **被混出的这条**自己的 mixTime / mixDuration 算。
//   2. TrackEntry 走 LIFO 对象池（Pool.h:49-67），`newTrackEntry()` 与 `reset()` 都**不复位**
//      `_mixBlend` 与 `_totalAlpha` ⇒ 复用到的旧对象带着上一次的 mixBlend。这里同样用池。
//   3. `applyMixingFrom()` 里 `MathUtil::max(0.0f, x)` 是 `a > b ? a : b`：x 为 NaN 时返回 NaN
//      （std::max 会返回 0）。
//
// 用法（与参考实现相同）：每帧 `Update(dt)` → `Apply(skeleton_state)` → `SpineGpuUpdateWorld()`。
// =====================================================================

#include "Runtime/Function/Render/Spine/SpineGpuSkeletonState.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <unordered_set>
#include <utility>
#include <vector>

// 空动画（`AnimationState::getEmptyAnimation()`，时长 0、没有时间轴）的"动画下标"。
constexpr uint32_t kSpineGpuEmptyAnimation = 0xFFFFFFFFu;

// `AnimationStateData`：两两动画之间的混合时长。
struct SpineGpuAnimationStateData
{
    float default_mix = 0.0f;
    // (from, to) → 秒。参考实现按**动画名**比较（AnimationStateData.cpp:83-85），同一骨架内
    // 动画名唯一 ⇒ 按下标等价。空动画用 kSpineGpuEmptyAnimation。
    std::map<std::pair<uint32_t, uint32_t>, float> mixes;

    void SetMix(uint32_t from, uint32_t to, float duration) { mixes[{from, to}] = duration; }
    // AnimationStateData.cpp:58-66
    float GetMix(uint32_t from, uint32_t to) const
    {
        const auto it = mixes.find({from, to});
        return it != mixes.end() ? it->second : default_mix;
    }
};

// `TrackEntry`：字段与参考实现一一对应（下划线前缀去掉、驼峰改下划线）。
// 公开字段 = 参考实现里有 setter 的那些，调用方可以直接改（如 `alpha`、`mix_duration`）。
struct SpineGpuTrackEntry
{
    uint32_t animation = kSpineGpuEmptyAnimation;
    SpineGpuTrackEntry* next = nullptr;
    SpineGpuTrackEntry* mixing_from = nullptr;
    SpineGpuTrackEntry* mixing_to = nullptr;
    uint32_t track_index = 0;
    bool loop = false;
    bool hold_previous = false;
    float event_threshold = 0.0f;
    float attachment_threshold = 0.0f;
    float draw_order_threshold = 0.0f;
    float animation_start = 0.0f;
    float animation_end = 0.0f;
    float animation_last = 0.0f;
    float next_animation_last = 0.0f;
    float delay = 0.0f;
    float track_time = 0.0f;
    float track_last = 0.0f;
    float next_track_last = 0.0f;
    float track_end = 0.0f;
    float time_scale = 1.0f;
    float alpha = 0.0f;
    float mix_time = 0.0f;
    float mix_duration = 0.0f;
    float interrupt_alpha = 0.0f;
    float total_alpha = 0.0f;
    SpineGpuMixBlend mix_blend = SpineGpuMixBlend::kReplace;

    std::vector<int32_t> timeline_mode;
    std::vector<SpineGpuTrackEntry*> timeline_hold_mix;
    std::vector<float> timelines_rotation;

    // TrackEntry::getAnimationTime()（AnimationState.cpp:104-115）
    float GetAnimationTime() const;
    // TrackEntry::isComplete()（:139-141）
    bool IsComplete() const { return track_time >= animation_end - animation_start; }
    // TrackEntry::setAnimationLast()（:99-102）
    void SetAnimationLast(float v)
    {
        animation_last = v;
        next_animation_last = v;
    }
    void ResetRotationDirections() { timelines_rotation.clear(); }
};

class SpineGpuAnimationState
{
public:
    // `asset` / `data` / `state_data` 必须比本对象活得久（与参考实现持有 AnimationStateData* 一样）。
    SpineGpuAnimationState(const SpineGpuAsset& asset, const SpineGpuRuntimeData& data,
                           const SpineGpuAnimationStateData* state_data);
    ~SpineGpuAnimationState();
    SpineGpuAnimationState(const SpineGpuAnimationState&) = delete;
    SpineGpuAnimationState& operator=(const SpineGpuAnimationState&) = delete;

    // 动画下标越界 ⇒ 返回 nullptr、不改任何状态（参考实现是 assert）。
    SpineGpuTrackEntry* SetAnimation(size_t track_index, uint32_t animation, bool loop);
    SpineGpuTrackEntry* AddAnimation(size_t track_index, uint32_t animation, bool loop, float delay);
    SpineGpuTrackEntry* SetEmptyAnimation(size_t track_index, float mix_duration);
    SpineGpuTrackEntry* AddEmptyAnimation(size_t track_index, float mix_duration, float delay);
    void SetEmptyAnimations(float mix_duration);
    void ClearTracks();
    void ClearTrack(size_t track_index);
    SpineGpuTrackEntry* GetCurrent(size_t track_index) const;
    size_t TrackCount() const { return m_Tracks.size(); }

    void Update(float delta);
    // 返回 !ok 只可能是"容器不自洽 / 遇到本阶段不做的时间轴"——此时骨架状态已被部分改写。
    // `applied` 非空时写入参考实现 `apply()` 的返回值（是否有任何轨道被应用）。
    SpineImportResult Apply(SpineGpuSkeletonState& skeleton, bool* applied = nullptr);

    float time_scale = 1.0f;

private:
    float GetDuration(uint32_t animation) const;
    void TimelineRange(uint32_t animation, uint32_t& first, uint32_t& count) const;

    SpineImportResult ApplyMixingFrom(SpineGpuTrackEntry* to, SpineGpuSkeletonState& skeleton, SpineGpuMixBlend blend,
                                      float& mix_out);
    bool UpdateMixingFrom(SpineGpuTrackEntry* to, float delta);
    void SetCurrent(size_t index, SpineGpuTrackEntry* current, bool interrupt);
    SpineGpuTrackEntry* ExpandToIndex(size_t index);
    SpineGpuTrackEntry* NewTrackEntry(size_t track_index, uint32_t animation, bool loop, SpineGpuTrackEntry* last);
    void DisposeNext(SpineGpuTrackEntry* entry);
    void AnimationsChanged();
    void SetTimelineModes(SpineGpuTrackEntry* entry);
    bool HasTimeline(const SpineGpuTrackEntry* entry, uint64_t id) const;

    // 事件队列只保留对状态的副作用（见文件头）。
    void QueueStart(SpineGpuTrackEntry*) { m_AnimationsChanged = true; }
    void QueueEnd(SpineGpuTrackEntry* e)
    {
        m_PendingFree.push_back(e);
        m_AnimationsChanged = true;
    }
    void QueueDispose(SpineGpuTrackEntry* e) { m_PendingFree.push_back(e); }
    void Drain();

    // 对象池（Pool.h）
    SpineGpuTrackEntry* Obtain();
    void Free(SpineGpuTrackEntry* e);

    const SpineGpuAsset& m_Asset;
    const SpineGpuRuntimeData& m_Data;
    const SpineGpuAnimationStateData* m_StateData;

    std::vector<SpineGpuTrackEntry*> m_Tracks;
    std::unordered_set<uint64_t> m_PropertyIds;   // 参考实现是 Vector<int> + 线性 contains：只问"在不在"，与顺序无关
    bool m_AnimationsChanged = false;

    std::vector<SpineGpuTrackEntry*> m_PendingFree;
    bool m_DrainDisabled = false;

    std::vector<std::unique_ptr<SpineGpuTrackEntry>> m_AllEntries;   // 所有权
    std::vector<SpineGpuTrackEntry*> m_FreeEntries;                  // 池里空闲的（LIFO）
};
