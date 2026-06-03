#include "TimelineDirector.h"

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Function/Animation/AnimationSystem.h"
#include "Runtime/Function/Framework/Component/Animation/AnimationComponent.h"
#include "Runtime/Function/Framework/Level/Level.h"
#include "Runtime/Resource/Asset/AssetManager.h"

#include <algorithm>

TimelineDirector::TimelineDirector() {}

TimelineDirector::~TimelineDirector() {}

bool TimelineDirector::Initialize(TimelineAsset* timeline_asset, GameObject* owner_object, Level* level)
{
    if (!timeline_asset || !owner_object || !level)
    {
        return false;
    }

    m_TimelineAsset = timeline_asset;
    m_OwnerObject = owner_object;
    m_Level = level;
    m_Initialized = true;

    // 重置状态
    m_CurrentTime = 0.0f;
    m_PlayState = TimelinePlayState::Stopped;
    m_EventTriggeredMap.clear();

    return true;
}

void TimelineDirector::Tick(float delta_time)
{
    if (!m_Initialized || !m_TimelineAsset)
        return;

    if (m_PlayState == TimelinePlayState::Playing)
    {
        // 更新当前时间
        m_CurrentTime += delta_time * m_Speed;

        // 检查是否到达结束
        float duration = m_TimelineAsset->m_Duration;
        if (m_CurrentTime >= duration)
        {
            if (m_Loop)
            {
                m_CurrentTime = 0.0f;
                m_EventTriggeredMap.clear();  // 重置事件触发状态
            }
            else
            {
                m_CurrentTime = duration;
                Stop();
            }
        }

        // 更新所有轨道
        UpdateTracks(m_CurrentTime);
    }
}

void TimelineDirector::Play()
{
    if (!m_Initialized)
        return;

    m_PlayState = TimelinePlayState::Playing;
}

void TimelineDirector::Pause()
{
    if (!m_Initialized)
        return;

    m_PlayState = TimelinePlayState::Paused;
}

void TimelineDirector::Stop()
{
    if (!m_Initialized)
        return;

    m_PlayState = TimelinePlayState::Stopped;
    m_CurrentTime = 0.0f;
    m_EventTriggeredMap.clear();
}

void TimelineDirector::SetTime(float time)
{
    if (!m_Initialized || !m_TimelineAsset)
        return;

    m_CurrentTime = std::clamp(time, 0.0f, m_TimelineAsset->m_Duration);
    UpdateTracks(m_CurrentTime);
}

void TimelineDirector::UpdateTracks(float time)
{
    if (!m_TimelineAsset)
        return;

    for (auto& track_ptr : m_TimelineAsset->m_Tracks)
    {
        if (!track_ptr || !track_ptr->m_Enabled || track_ptr->m_Muted)
            continue;

        UpdateTrack(track_ptr, time);
    }
}

void TimelineDirector::UpdateTrack(TimelineTrack* track, float time)
{
    if (!track)
        return;

    GameObject* target_object = GetTargetObject(track->m_TargetObjectId);
    if (!target_object)
        return;

    // 遍历轨道上的所有片段
    for (auto& clip_ptr : track->m_Clips)
    {
        if (!clip_ptr || !clip_ptr->m_Enabled)
            continue;

        TimelineClip* clip = clip_ptr;

        // 检查时间是否在片段范围内
        if (!clip->containsTime(time))
            continue;

        // 计算归一化时间
        float normalized_time = clip->getNormalizedTime(time);

        // 根据片段类型更新
        if (auto* anim_clip = dynamic_cast<AnimationTimelineClip*>(clip))
        {
            UpdateAnimationClip(anim_clip, normalized_time, target_object);
        }
        else if (auto* activation_clip = dynamic_cast<ActivationTimelineClip*>(clip))
        {
            UpdateActivationClip(activation_clip, target_object);
        }
        else if (auto* audio_clip = dynamic_cast<AudioTimelineClip*>(clip))
        {
            UpdateAudioClip(audio_clip, normalized_time, target_object);
        }
        else if (auto* event_clip = dynamic_cast<EventTimelineClip*>(clip))
        {
            UpdateEventClip(event_clip, target_object);
        }
    }
}

void TimelineDirector::UpdateAnimationClip(AnimationTimelineClip* clip,
                                           float normalized_time,
                                           GameObject* target_object)
{
    if (!clip || !target_object)
        return;

    // 获取动画组件
    auto* anim_component = target_object->tryGetComponent<AnimationComponent>("AnimationComponent");
    if (!anim_component)
        return;

    // TODO: 实现动画片段的更新逻辑
    // 这里需要根据normalized_time和clip的配置来更新动画组件
    // 可能需要加载动画资源并设置到动画组件上
}

void TimelineDirector::UpdateActivationClip(ActivationTimelineClip* clip, GameObject* target_object)
{
    if (!clip || !target_object)
        return;

    // TODO: 实现GameObject激活状态的更新
    // 注意：ZEngine的GameObject可能没有直接的激活/禁用接口
    // 可能需要通过组件来实现
}

void TimelineDirector::UpdateAudioClip(AudioTimelineClip* clip, float normalized_time, GameObject* target_object)
{
    if (!clip || !target_object)
        return;

    // TODO: 实现音频播放逻辑
    // 需要音频系统的支持
}

void TimelineDirector::UpdateEventClip(EventTimelineClip* clip, GameObject* target_object)
{
    if (!clip || !target_object)
        return;

    // 检查事件是否已经触发过（在同一时间点）
    if (m_EventTriggeredMap.find(clip) != m_EventTriggeredMap.end() && m_EventTriggeredMap[clip])
        return;

    // 标记事件已触发
    m_EventTriggeredMap[clip] = true;

    // TODO: 触发事件
    // 可以通过事件系统或回调机制来实现
    // LOG_INFO("Timeline Event: {} with params: {}", clip->m_EventName, clip->m_EventParams);
}

GameObject* TimelineDirector::GetTargetObject(uint64_t object_id)
{
    if (object_id == 0)
    {
        return m_OwnerObject;
    }

    if (!m_Level)
        return nullptr;

    // 从Level中获取GameObject
    auto go_weak = m_Level->GetGObjectByID(static_cast<GObjectID>(object_id));
    if (auto go_shared = go_weak.lock())
    {
        return go_shared.get();
    }

    return nullptr;
}