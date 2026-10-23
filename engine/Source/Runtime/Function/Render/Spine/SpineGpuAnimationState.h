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
// 事件（EventQueue，AnimationState.cpp:203-271 / queueEvents :842-878）：start / interrupt / end / dispose /
// complete 照参考实现的入队点与顺序转录，`Drain()` 时按入队顺序写进 `event_sink`（为空 ⇒ 不记，行为不变）。
// end 之后紧跟一条 dispose（参考实现 drain 的 case 贯穿），二者都把 TrackEntry 还回对象池。
// 用户事件（EventType_Event，v16）：event 时间轴（type 7）在 apply 时按 `EventTimeline::apply`
// （EventTimeline.cpp:63-103，含循环回绕的递归与同一时刻多事件的回退）收进 `_events`（m_Events），
// `queueEvents` 再按 complete 前 / 后两段、animationStart / End 裁剪入队；被混出的那条只在
// `mix < eventThreshold` 时收（applyMixingFrom :765），且只在 `to->mixDuration > 0` 时入队。
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

// spine::EventType（AnimationState.h），值与参考实现的枚举顺序一致。
enum class SpineGpuEventType : uint8_t
{
    kStart = 0,
    kInterrupt = 1,
    kEnd = 2,
    kComplete = 3,
    kDispose = 4,
    kEvent = 5,   // 用户事件（event 时间轴，v16）
};

// 一次触发的用户事件 = `spine::Event` 的值拷贝（时间轴载荷 SpineGpuEventKey + 帧时刻）。
// 字符串 / 名字是进 SpineGpuAsset::event_strings 的下标；定义（名字 / 音频）走 `event`。
struct SpineGpuUserEvent
{
    uint32_t event = 0;          // 进 SpineGpuAsset::events 的下标
    int32_t int_value = 0;
    float float_value = 0.0f;
    uint32_t string_value = 0;   // 进 SpineGpuAsset::event_strings 的下标
    float volume = 1.0f;
    float balance = 0.0f;
    float time = 0.0f;           // Event::getTime()（动画时间，不是 trackTime）
};

// 一条已派发的事件（Drain 时从 TrackEntry 抄出来：派发之后那条 entry 可能已回到池里）。
struct SpineGpuAnimationEvent
{
    SpineGpuEventType type = SpineGpuEventType::kStart;
    uint32_t track = 0;
    uint32_t animation = kSpineGpuEmptyAnimation;
    bool loop = false;
    float track_time = 0.0f;
    SpineGpuUserEvent user;      // 只在 type == kEvent 时有意义
};

const char* SpineGpuEventTypeName(SpineGpuEventType type);

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
    // 非空 ⇒ 每次 Drain 把派发的事件**追加**进来（调用方负责清空 / 取走）。
    std::vector<SpineGpuAnimationEvent>* event_sink = nullptr;

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

    // EventQueue（AnimationState.cpp:203-228）
    void QueueStart(SpineGpuTrackEntry* e)
    {
        m_Queue.push_back({SpineGpuEventType::kStart, e});
        m_AnimationsChanged = true;
    }
    void QueueInterrupt(SpineGpuTrackEntry* e) { m_Queue.push_back({SpineGpuEventType::kInterrupt, e}); }
    void QueueEnd(SpineGpuTrackEntry* e)
    {
        m_Queue.push_back({SpineGpuEventType::kEnd, e});
        m_AnimationsChanged = true;
    }
    void QueueDispose(SpineGpuTrackEntry* e) { m_Queue.push_back({SpineGpuEventType::kDispose, e}); }
    void QueueComplete(SpineGpuTrackEntry* e) { m_Queue.push_back({SpineGpuEventType::kComplete, e}); }
    void QueueUserEvent(SpineGpuTrackEntry* e, const SpineGpuUserEvent& ev)
    {
        m_Queue.push_back({SpineGpuEventType::kEvent, e, ev});
    }
    // queueEvents（:842-878）
    void QueueEvents(SpineGpuTrackEntry* entry, float animation_time);
    // EventTimeline::apply（EventTimeline.cpp:63-103）：把 (last_time, time] 里的事件追加到 out
    void CollectEventTimeline(uint32_t tl_index, float last_time, float time, std::vector<SpineGpuUserEvent>& out) const;
    // `timeline->apply(skeleton, lastTime, time, eventBuffer, ...)` 里事件时间轴那一半：buffer 为空 ⇒ 不收
    void CollectIfEvent(uint32_t tl_index, float last_time, float time, std::vector<SpineGpuUserEvent>* buffer) const
    {
        if (buffer != nullptr &&
            static_cast<SpineTimelineType>(m_Asset.timelines[tl_index].type) == SpineTimelineType::kEvent)
        {
            CollectEventTimeline(tl_index, last_time, time, *buffer);
        }
    }
    void Drain();
    void Emit(SpineGpuEventType type, const SpineGpuTrackEntry& e, const SpineGpuUserEvent* user = nullptr);

    // 对象池（Pool.h）
    SpineGpuTrackEntry* Obtain();
    void Free(SpineGpuTrackEntry* e);

    const SpineGpuAsset& m_Asset;
    const SpineGpuRuntimeData& m_Data;
    const SpineGpuAnimationStateData* m_StateData;

    std::vector<SpineGpuTrackEntry*> m_Tracks;
    std::unordered_set<uint64_t> m_PropertyIds;   // 参考实现是 Vector<int> + 线性 contains：只问"在不在"，与顺序无关
    bool m_AnimationsChanged = false;

    struct QueuedEvent
    {
        SpineGpuEventType type;
        SpineGpuTrackEntry* entry;
        SpineGpuUserEvent user {};   // 只有 kEvent 用
    };
    std::vector<QueuedEvent> m_Queue;
    std::vector<SpineGpuUserEvent> m_Events;   // 参考实现的 `_events`：一条轨道 apply 期间收集，queueEvents 后清空
    bool m_DrainDisabled = false;

    std::vector<std::unique_ptr<SpineGpuTrackEntry>> m_AllEntries;   // 所有权
    std::vector<SpineGpuTrackEntry*> m_FreeEntries;                  // 池里空闲的（LIFO）
};
