#include "Runtime/Function/Render/Spine/SpineGpuAnimationState.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

// =====================================================================
// 参考实现：G:/OSGame/unity-engine-source/Modules/Spine/spine-cpp/src/spine/AnimationState.cpp（3.8）
// 行号都指这个文件。MathUtil::min / max 是 `a < b ? a : b` / `a > b ? a : b`（MathUtil.h:50-53），
// 与 std::min / std::max 在 NaN 上不同 ⇒ 用下面两个本地版本。
// =====================================================================

namespace
{

// `const int Subsequent = 0, First = 1, Hold = 2, HoldMix = 3;`（:273-276）
constexpr int32_t kSubsequent = 0;
constexpr int32_t kFirst = 1;
constexpr int32_t kHold = 2;
constexpr int32_t kHoldMix = 3;

inline float MinF(float a, float b) { return a < b ? a : b; }
inline float MaxF(float a, float b) { return a > b ? a : b; }

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

// TrackEntry::reset()（:167-180）
void ResetEntry(SpineGpuTrackEntry& e)
{
    e.animation = kSpineGpuEmptyAnimation;
    e.next = nullptr;
    e.mixing_from = nullptr;
    e.mixing_to = nullptr;
    e.timeline_mode.clear();
    e.timeline_hold_mix.clear();
    e.timelines_rotation.clear();
}

}   // namespace

float SpineGpuTrackEntry::GetAnimationTime() const
{
    if (loop)
    {
        const float duration = animation_end - animation_start;
        if (duration == 0.0f)
        {
            return animation_start;
        }
        // `MathUtil::fmod` = `(float)::fmod(a, b)`；fmod 结果在 float 上精确可表示 ⇒ float 版逐位相同
        return std::fmod(track_time, duration) + animation_start;
    }
    return MinF(track_time + animation_start, animation_end);
}

SpineGpuAnimationState::SpineGpuAnimationState(const SpineGpuAsset& asset, const SpineGpuRuntimeData& data,
                                               const SpineGpuAnimationStateData* state_data)
    : m_Asset(asset), m_Data(data), m_StateData(state_data)
{
}

SpineGpuAnimationState::~SpineGpuAnimationState() = default;

float SpineGpuAnimationState::GetDuration(uint32_t animation) const
{
    if (animation == kSpineGpuEmptyAnimation || animation >= m_Asset.anims.size())
    {
        return 0.0f;
    }
    return m_Asset.anims[animation].duration;
}

void SpineGpuAnimationState::TimelineRange(uint32_t animation, uint32_t& first, uint32_t& count) const
{
    first = 0;
    count = 0;
    if (animation == kSpineGpuEmptyAnimation || animation >= m_Asset.anims.size())
    {
        return;
    }
    first = m_Asset.anims[animation].timeline_first;
    count = m_Asset.anims[animation].timeline_count;
}

// ---------------------------------------------------------------------
// 对象池 / 事件队列
// ---------------------------------------------------------------------

SpineGpuTrackEntry* SpineGpuAnimationState::Obtain()
{
    // Pool::obtain()（Pool.h:49-61）：LIFO；新建的走 TrackEntry 构造函数的初值（:55-61）
    if (!m_FreeEntries.empty())
    {
        SpineGpuTrackEntry* e = m_FreeEntries.back();
        m_FreeEntries.pop_back();
        return e;
    }
    m_AllEntries.push_back(std::make_unique<SpineGpuTrackEntry>());
    return m_AllEntries.back().get();
}

void SpineGpuAnimationState::Free(SpineGpuTrackEntry* e)
{
    // Pool::free()（Pool.h:63-67）：已在池里就不重复放
    if (std::find(m_FreeEntries.begin(), m_FreeEntries.end(), e) == m_FreeEntries.end())
    {
        m_FreeEntries.push_back(e);
    }
}

void SpineGpuAnimationState::Drain()
{
    // EventQueue::drain()（:231-271）：end / dispose ⇒ reset + 还回池；其余事件只有回调，不做。
    if (m_DrainDisabled)
    {
        return;
    }
    m_DrainDisabled = true;
    for (size_t i = 0; i < m_PendingFree.size(); ++i)
    {
        SpineGpuTrackEntry* e = m_PendingFree[i];
        ResetEntry(*e);
        Free(e);
    }
    m_PendingFree.clear();
    m_DrainDisabled = false;
}

// ---------------------------------------------------------------------
// update（:308-371）
// ---------------------------------------------------------------------

void SpineGpuAnimationState::Update(float delta)
{
    delta *= time_scale;
    for (size_t i = 0, n = m_Tracks.size(); i < n; ++i)
    {
        SpineGpuTrackEntry* current_p = m_Tracks[i];
        if (current_p == nullptr)
        {
            continue;
        }
        SpineGpuTrackEntry& current = *current_p;

        current.animation_last = current.next_animation_last;
        current.track_last = current.next_track_last;

        float current_delta = delta * current.time_scale;

        if (current.delay > 0.0f)
        {
            current.delay -= current_delta;
            if (current.delay > 0.0f)
            {
                continue;
            }
            current_delta = -current.delay;
            current.delay = 0.0f;
        }

        SpineGpuTrackEntry* next = current.next;
        if (next != nullptr)
        {
            // 下一条的 delay 到了 ⇒ 切过去，保留多出来的时间
            const float next_time = current.track_last - next->delay;
            if (next_time >= 0.0f)
            {
                next->delay = 0.0f;
                next->track_time =
                    current.time_scale == 0.0f ? 0.0f : (next_time / current.time_scale + delta) * next->time_scale;
                current.track_time += current_delta;
                SetCurrent(i, next, true);
                while (next->mixing_from != nullptr)
                {
                    next->mix_time += delta;
                    next = next->mixing_from;
                }
                continue;
            }
        }
        else if (current.track_last >= current.track_end && current.mixing_from == nullptr)
        {
            // 没有下一条、到了 trackEnd、也没在混 ⇒ 清掉这条轨道
            m_Tracks[i] = nullptr;
            QueueEnd(current_p);
            DisposeNext(current_p);
            continue;
        }

        if (current.mixing_from != nullptr && UpdateMixingFrom(current_p, delta))
        {
            // 所有 mixingFrom 都完成 ⇒ 结束混合
            SpineGpuTrackEntry* from = current.mixing_from;
            current.mixing_from = nullptr;
            if (from != nullptr)
            {
                from->mixing_to = nullptr;
            }
            while (from != nullptr)
            {
                QueueEnd(from);
                from = from->mixing_from;
            }
        }

        current.track_time += current_delta;
    }
    Drain();
}

// updateMixingFrom（:717-744）
bool SpineGpuAnimationState::UpdateMixingFrom(SpineGpuTrackEntry* to, float delta)
{
    SpineGpuTrackEntry* from = to->mixing_from;
    if (from == nullptr)
    {
        return true;
    }

    const bool finished = UpdateMixingFrom(from, delta);

    from->animation_last = from->next_animation_last;
    from->track_last = from->next_track_last;

    // mixTime > 0 保证被混出的那条至少应用过一次
    if (to->mix_time > 0.0f && to->mix_time >= to->mix_duration)
    {
        // totalAlpha == 0 才算混完，除非 mixDuration == 0（单帧过渡）
        if (from->total_alpha == 0.0f || to->mix_duration == 0.0f)
        {
            to->mixing_from = from->mixing_from;
            if (from->mixing_from != nullptr)
            {
                from->mixing_from->mixing_to = to;
            }
            to->interrupt_alpha = from->interrupt_alpha;
            QueueEnd(from);
        }
        return finished;
    }

    from->track_time += delta * from->time_scale;
    to->mix_time += delta;
    return false;
}

// ---------------------------------------------------------------------
// apply（:373-444）
// ---------------------------------------------------------------------

SpineImportResult SpineGpuAnimationState::Apply(SpineGpuSkeletonState& skeleton, bool* applied_out)
{
    if (m_AnimationsChanged)
    {
        AnimationsChanged();
    }

    bool applied = false;
    for (size_t i = 0, n = m_Tracks.size(); i < n; ++i)
    {
        SpineGpuTrackEntry* current_p = m_Tracks[i];
        if (current_p == nullptr || current_p->delay > 0.0f)
        {
            continue;
        }
        SpineGpuTrackEntry& current = *current_p;

        applied = true;
        const SpineGpuMixBlend blend = i == 0 ? SpineGpuMixBlend::kFirst : current.mix_blend;

        // 先应用 mixingFrom
        float mix = current.alpha;
        if (current.mixing_from != nullptr)
        {
            float m = 0.0f;
            const SpineImportResult r = ApplyMixingFrom(current_p, skeleton, blend, m);
            if (!r.ok)
            {
                return r;
            }
            mix *= m;
        }
        else if (current.track_time >= current.track_end && current.next == nullptr)
        {
            mix = 0.0f;   // 这条最后一次被应用 ⇒ 复位到 setup
        }

        // 再应用当前这条
        const float animation_last = current.animation_last;
        const float animation_time = current.GetAnimationTime();
        uint32_t tl_first = 0;
        uint32_t tl_count = 0;
        TimelineRange(current.animation, tl_first, tl_count);
        if ((i == 0 && mix == 1.0f) || blend == SpineGpuMixBlend::kAdd)
        {
            for (uint32_t ii = 0; ii < tl_count; ++ii)
            {
                const SpineImportResult r = SpineGpuApplyTimeline(m_Asset, m_Data, skeleton, tl_first + ii,
                                                                  animation_last, animation_time, mix, blend,
                                                                  SpineGpuMixDirection::kIn);
                if (!r.ok)
                {
                    return r;
                }
            }
        }
        else
        {
            if (current.timeline_mode.size() != tl_count)
            {
                return Fail("animation-state: 轨道 " + std::to_string(i) + " 的 timelineMode 长度 " +
                            std::to_string(current.timeline_mode.size()) + " 与时间轴数 " + std::to_string(tl_count) +
                            " 不一致（参考实现在这里是越界读）");
            }
            const bool first_frame = current.timelines_rotation.empty();
            if (first_frame)
            {
                current.timelines_rotation.assign(static_cast<size_t>(tl_count) << 1, 0.0f);
            }
            for (uint32_t ii = 0; ii < tl_count; ++ii)
            {
                const uint32_t tl_index = tl_first + ii;
                const SpineGpuMixBlend timeline_blend =
                    current.timeline_mode[ii] == kSubsequent ? blend : SpineGpuMixBlend::kSetup;
                SpineImportResult r;
                if (static_cast<SpineTimelineType>(m_Asset.timelines[tl_index].type) == SpineTimelineType::kRotate)
                {
                    r = SpineGpuApplyRotateTimelineMixed(m_Asset, skeleton, tl_index, animation_time, mix,
                                                         timeline_blend,
                                                         current.timelines_rotation.data() + (static_cast<size_t>(ii) << 1),
                                                         first_frame);
                }
                else
                {
                    r = SpineGpuApplyTimeline(m_Asset, m_Data, skeleton, tl_index, animation_last, animation_time, mix,
                                              timeline_blend, SpineGpuMixDirection::kIn);
                }
                if (!r.ok)
                {
                    return r;
                }
            }
        }

        current.next_animation_last = animation_time;
        current.next_track_last = current.track_time;
    }

    Drain();
    if (applied_out != nullptr)
    {
        *applied_out = applied;
    }
    return Ok();
}

// applyMixingFrom（:746-840）
SpineImportResult SpineGpuAnimationState::ApplyMixingFrom(SpineGpuTrackEntry* to, SpineGpuSkeletonState& skeleton,
                                                          SpineGpuMixBlend blend, float& mix_out)
{
    SpineGpuTrackEntry* from = to->mixing_from;
    if (from->mixing_from != nullptr)
    {
        float ignored = 0.0f;   // 参考实现丢弃了这个返回值
        const SpineImportResult r = ApplyMixingFrom(from, skeleton, blend, ignored);
        if (!r.ok)
        {
            return r;
        }
    }

    float mix;
    if (to->mix_duration == 0.0f)
    {
        // 单帧混合：撤销 mixingFrom 的改动
        mix = 1.0f;
        if (blend == SpineGpuMixBlend::kFirst)
        {
            blend = SpineGpuMixBlend::kSetup;
        }
    }
    else
    {
        mix = to->mix_time / to->mix_duration;
        if (mix > 1.0f)
        {
            mix = 1.0f;
        }
        if (blend != SpineGpuMixBlend::kFirst)
        {
            blend = from->mix_blend;
        }
    }

    const bool attachments = mix < from->attachment_threshold;
    const bool draw_order = mix < from->draw_order_threshold;
    const float animation_last = from->animation_last;
    const float animation_time = from->GetAnimationTime();
    uint32_t tl_first = 0;
    uint32_t tl_count = 0;
    TimelineRange(from->animation, tl_first, tl_count);
    const float alpha_hold = from->alpha * to->interrupt_alpha;
    const float alpha_mix = alpha_hold * (1.0f - mix);

    if (blend == SpineGpuMixBlend::kAdd)
    {
        for (uint32_t i = 0; i < tl_count; ++i)
        {
            const SpineImportResult r = SpineGpuApplyTimeline(m_Asset, m_Data, skeleton, tl_first + i, animation_last,
                                                              animation_time, alpha_mix, blend,
                                                              SpineGpuMixDirection::kOut);
            if (!r.ok)
            {
                return r;
            }
        }
    }
    else
    {
        if (from->timeline_mode.size() != tl_count || from->timeline_hold_mix.size() != tl_count)
        {
            return Fail("animation-state: 被混出的那条的 timelineMode 长度与时间轴数不一致（参考实现在这里是越界读）");
        }
        const bool first_frame = from->timelines_rotation.empty();
        if (first_frame)
        {
            from->timelines_rotation.assign(static_cast<size_t>(tl_count) << 1, 0.0f);
        }

        from->total_alpha = 0.0f;
        for (uint32_t i = 0; i < tl_count; ++i)
        {
            const uint32_t tl_index = tl_first + i;
            const SpineTimelineType type = static_cast<SpineTimelineType>(m_Asset.timelines[tl_index].type);
            SpineGpuMixDirection direction = SpineGpuMixDirection::kOut;
            SpineGpuMixBlend timeline_blend;
            float alpha;
            switch (from->timeline_mode[i])
            {
                case kSubsequent:
                    if (!attachments && type == SpineTimelineType::kAttachment)
                    {
                        continue;
                    }
                    if (!draw_order && type == SpineTimelineType::kDrawOrder)
                    {
                        continue;
                    }
                    timeline_blend = blend;
                    alpha = alpha_mix;
                    break;
                case kFirst:
                    timeline_blend = SpineGpuMixBlend::kSetup;
                    alpha = alpha_mix;
                    break;
                case kHold:
                    timeline_blend = SpineGpuMixBlend::kSetup;
                    alpha = alpha_hold;
                    break;
                default:
                {
                    timeline_blend = SpineGpuMixBlend::kSetup;
                    const SpineGpuTrackEntry* hold_mix = from->timeline_hold_mix[i];
                    if (hold_mix == nullptr)
                    {
                        return Fail("animation-state: HoldMix 的 timelineHoldMix 为空（参考实现在这里是空指针解引用）");
                    }
                    alpha = alpha_hold * MaxF(0.0f, 1.0f - hold_mix->mix_time / hold_mix->mix_duration);
                    break;
                }
            }
            from->total_alpha += alpha;
            SpineImportResult r;
            if (type == SpineTimelineType::kRotate)
            {
                r = SpineGpuApplyRotateTimelineMixed(m_Asset, skeleton, tl_index, animation_time, alpha, timeline_blend,
                                                     from->timelines_rotation.data() + (static_cast<size_t>(i) << 1),
                                                     first_frame);
            }
            else
            {
                if (timeline_blend == SpineGpuMixBlend::kSetup)
                {
                    if (type == SpineTimelineType::kAttachment)
                    {
                        if (attachments)
                        {
                            direction = SpineGpuMixDirection::kIn;
                        }
                    }
                    else if (type == SpineTimelineType::kDrawOrder)
                    {
                        if (draw_order)
                        {
                            direction = SpineGpuMixDirection::kIn;
                        }
                    }
                }
                r = SpineGpuApplyTimeline(m_Asset, m_Data, skeleton, tl_index, animation_last, animation_time, alpha,
                                          timeline_blend, direction);
            }
            if (!r.ok)
            {
                return r;
            }
        }
    }

    from->next_animation_last = animation_time;
    from->next_track_last = from->track_time;
    mix_out = mix;
    return Ok();
}

// ---------------------------------------------------------------------
// 轨道管理
// ---------------------------------------------------------------------

// clearTracks（:446-455）
void SpineGpuAnimationState::ClearTracks()
{
    const bool old_drain_disabled = m_DrainDisabled;
    m_DrainDisabled = true;
    for (size_t i = 0, n = m_Tracks.size(); i < n; ++i)
    {
        ClearTrack(i);
    }
    m_Tracks.clear();
    m_DrainDisabled = old_drain_disabled;
    Drain();
}

// clearTrack（:457-487）
void SpineGpuAnimationState::ClearTrack(size_t track_index)
{
    if (track_index >= m_Tracks.size())
    {
        return;
    }
    SpineGpuTrackEntry* current = m_Tracks[track_index];
    if (current == nullptr)
    {
        return;
    }

    QueueEnd(current);
    DisposeNext(current);

    SpineGpuTrackEntry* entry = current;
    while (true)
    {
        SpineGpuTrackEntry* from = entry->mixing_from;
        if (from == nullptr)
        {
            break;
        }
        QueueEnd(from);
        entry->mixing_from = nullptr;
        entry->mixing_to = nullptr;
        entry = from;
    }

    m_Tracks[current->track_index] = nullptr;
    Drain();
}

// setAnimation（:499-523）
SpineGpuTrackEntry* SpineGpuAnimationState::SetAnimation(size_t track_index, uint32_t animation, bool loop)
{
    if (animation != kSpineGpuEmptyAnimation && animation >= m_Asset.anims.size())
    {
        return nullptr;
    }
    bool interrupt = true;
    SpineGpuTrackEntry* current = ExpandToIndex(track_index);
    if (current != nullptr)
    {
        if (current->next_track_last == -1.0f)
        {
            // 从没被应用过的那条不参与混合
            m_Tracks[track_index] = current->mixing_from;
            QueueEnd(current);
            DisposeNext(current);
            current = current->mixing_from;
            interrupt = false;
        }
        else
        {
            DisposeNext(current);
        }
    }

    SpineGpuTrackEntry* entry = NewTrackEntry(track_index, animation, loop, current);
    SetCurrent(track_index, entry, interrupt);
    Drain();
    return entry;
}

// addAnimation（:532-566）
SpineGpuTrackEntry* SpineGpuAnimationState::AddAnimation(size_t track_index, uint32_t animation, bool loop, float delay)
{
    if (animation != kSpineGpuEmptyAnimation && animation >= m_Asset.anims.size())
    {
        return nullptr;
    }
    SpineGpuTrackEntry* last = ExpandToIndex(track_index);
    if (last != nullptr)
    {
        while (last->next != nullptr)
        {
            last = last->next;
        }
    }

    SpineGpuTrackEntry* entry = NewTrackEntry(track_index, animation, loop, last);

    if (last == nullptr)
    {
        SetCurrent(track_index, entry, true);
        Drain();
    }
    else
    {
        last->next = entry;
        if (delay <= 0.0f)
        {
            const float duration = last->animation_end - last->animation_start;
            if (duration != 0.0f)
            {
                if (last->loop)
                {
                    delay += duration * static_cast<float>(1 + static_cast<int>(last->track_time / duration));
                }
                else
                {
                    delay += MaxF(duration, last->track_time);
                }
                delay -= m_StateData != nullptr ? m_StateData->GetMix(last->animation, animation) : 0.0f;
            }
            else
            {
                delay = last->track_time;
            }
        }
    }

    entry->delay = delay;
    return entry;
}

// setEmptyAnimation（:568-573）
SpineGpuTrackEntry* SpineGpuAnimationState::SetEmptyAnimation(size_t track_index, float mix_duration)
{
    SpineGpuTrackEntry* entry = SetAnimation(track_index, kSpineGpuEmptyAnimation, false);
    entry->mix_duration = mix_duration;
    entry->track_end = mix_duration;
    return entry;
}

// addEmptyAnimation（:575-584）
SpineGpuTrackEntry* SpineGpuAnimationState::AddEmptyAnimation(size_t track_index, float mix_duration, float delay)
{
    if (delay <= 0.0f)
    {
        delay -= mix_duration;
    }
    SpineGpuTrackEntry* entry = AddAnimation(track_index, kSpineGpuEmptyAnimation, false, delay);
    entry->mix_duration = mix_duration;
    entry->track_end = mix_duration;
    return entry;
}

// setEmptyAnimations（:586-597）
void SpineGpuAnimationState::SetEmptyAnimations(float mix_duration)
{
    const bool old_drain_disabled = m_DrainDisabled;
    m_DrainDisabled = true;
    for (size_t i = 0, n = m_Tracks.size(); i < n; ++i)
    {
        if (m_Tracks[i] != nullptr)
        {
            SetEmptyAnimation(i, mix_duration);
        }
    }
    m_DrainDisabled = old_drain_disabled;
    Drain();
}

SpineGpuTrackEntry* SpineGpuAnimationState::GetCurrent(size_t track_index) const
{
    return track_index >= m_Tracks.size() ? nullptr : m_Tracks[track_index];
}

// setCurrent（:880-902）
void SpineGpuAnimationState::SetCurrent(size_t index, SpineGpuTrackEntry* current, bool interrupt)
{
    (void)interrupt;   // 只决定发不发 interrupt 事件
    SpineGpuTrackEntry* from = ExpandToIndex(index);
    m_Tracks[index] = current;

    if (from != nullptr)
    {
        current->mixing_from = from;
        from->mixing_to = current;
        current->mix_time = 0.0f;

        // 记下被打断时的混合百分比
        if (from->mixing_from != nullptr && from->mix_duration > 0.0f)
        {
            current->interrupt_alpha *= MinF(1.0f, from->mix_time / from->mix_duration);
        }

        from->timelines_rotation.clear();   // 混出时重新定旋转方向（它可能是混入一半被打断的）
    }

    QueueStart(current);   // ⇒ animationsChanged
}

// expandToIndex（:904-914）
SpineGpuTrackEntry* SpineGpuAnimationState::ExpandToIndex(size_t index)
{
    if (index < m_Tracks.size())
    {
        return m_Tracks[index];
    }
    while (index >= m_Tracks.size())
    {
        m_Tracks.push_back(nullptr);
    }
    return nullptr;
}

// newTrackEntry（:916-947）
//
// ⚠️ 不复位 `mix_blend` / `total_alpha`：参考实现的 newTrackEntry 与 reset() 都不碰它们，
//    池里复用到的旧对象带着上一次的值（见头文件第 2 条）。
SpineGpuTrackEntry* SpineGpuAnimationState::NewTrackEntry(size_t track_index, uint32_t animation, bool loop,
                                                          SpineGpuTrackEntry* last)
{
    SpineGpuTrackEntry& entry = *Obtain();

    entry.track_index = static_cast<uint32_t>(track_index);
    entry.animation = animation;
    entry.loop = loop;
    entry.hold_previous = false;

    entry.event_threshold = 0.0f;
    entry.attachment_threshold = 0.0f;
    entry.draw_order_threshold = 0.0f;

    entry.animation_start = 0.0f;
    entry.animation_end = GetDuration(animation);
    entry.animation_last = -1.0f;
    entry.next_animation_last = -1.0f;

    entry.delay = 0.0f;
    entry.track_time = 0.0f;
    entry.track_last = -1.0f;
    entry.next_track_last = -1.0f;   // -1 = 这条还没被应用过
    entry.track_end = std::numeric_limits<float>::max();
    entry.time_scale = 1.0f;

    entry.alpha = 1.0f;
    entry.interrupt_alpha = 1.0f;
    entry.mix_time = 0.0f;
    entry.mix_duration =
        (last == nullptr || m_StateData == nullptr) ? 0.0f : m_StateData->GetMix(last->animation, animation);
    return &entry;
}

// disposeNext（:949-956）
void SpineGpuAnimationState::DisposeNext(SpineGpuTrackEntry* entry)
{
    SpineGpuTrackEntry* next = entry->next;
    while (next != nullptr)
    {
        QueueDispose(next);
        next = next->next;
    }
    entry->next = nullptr;
}

// ---------------------------------------------------------------------
// timelineMode（:958-1033）
// ---------------------------------------------------------------------

// animationsChanged（:958-976）
void SpineGpuAnimationState::AnimationsChanged()
{
    m_AnimationsChanged = false;
    m_PropertyIds.clear();

    for (size_t i = 0, n = m_Tracks.size(); i < n; ++i)
    {
        SpineGpuTrackEntry* entry = m_Tracks[i];
        if (entry == nullptr)
        {
            continue;
        }
        while (entry->mixing_from != nullptr)
        {
            entry = entry->mixing_from;
        }
        do
        {
            if (entry->mixing_to == nullptr || entry->mix_blend != SpineGpuMixBlend::kAdd)
            {
                SetTimelineModes(entry);
            }
            entry = entry->mixing_to;
        } while (entry != nullptr);
    }
}

// setTimelineModes（:978-1023）
void SpineGpuAnimationState::SetTimelineModes(SpineGpuTrackEntry* entry)
{
    SpineGpuTrackEntry* to = entry->mixing_to;
    uint32_t tl_first = 0;
    uint32_t tl_count = 0;
    TimelineRange(entry->animation, tl_first, tl_count);
    entry->timeline_mode.assign(tl_count, 0);
    entry->timeline_hold_mix.assign(tl_count, nullptr);

    const auto contains = [&](uint64_t id) { return m_PropertyIds.count(id) != 0; };

    if (to != nullptr && to->hold_previous)
    {
        for (uint32_t i = 0; i < tl_count; ++i)
        {
            const uint64_t id = m_Data.property_key[tl_first + i];
            m_PropertyIds.insert(id);
            entry->timeline_mode[i] = kHold;
        }
        return;
    }

    for (uint32_t i = 0; i < tl_count; ++i)
    {
        const uint64_t id = m_Data.property_key[tl_first + i];
        if (contains(id))
        {
            entry->timeline_mode[i] = kSubsequent;
            continue;
        }
        m_PropertyIds.insert(id);

        if (to == nullptr || !HasTimeline(to, id))
        {
            entry->timeline_mode[i] = kFirst;
            continue;
        }
        // `goto continue_outer` 的等价写法：找到 HoldMix 就跳过后面的 `= Hold`
        bool hold_mix = false;
        for (SpineGpuTrackEntry* next = to->mixing_to; next != nullptr; next = next->mixing_to)
        {
            if (HasTimeline(next, id))
            {
                continue;
            }
            if (entry->mix_duration > 0.0f)
            {
                entry->timeline_mode[i] = kHoldMix;
                entry->timeline_hold_mix[i] = entry;   // ⚠️ 参考实现写的是 entry（见头文件第 1 条）
                hold_mix = true;
            }
            break;
        }
        if (!hold_mix)
        {
            entry->timeline_mode[i] = kHold;
        }
    }
}

// hasTimeline（:1025-1033）
bool SpineGpuAnimationState::HasTimeline(const SpineGpuTrackEntry* entry, uint64_t id) const
{
    uint32_t tl_first = 0;
    uint32_t tl_count = 0;
    TimelineRange(entry->animation, tl_first, tl_count);
    for (uint32_t i = 0; i < tl_count; ++i)
    {
        if (m_Data.property_key[tl_first + i] == id)
        {
            return true;
        }
    }
    return false;
}
