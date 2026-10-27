#include "TimelineDirector.h"

#include "Runtime/BaseClasses/Cast.h"
#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Function/Audio/AudioSystem.h"
#include "Runtime/Function/Framework/Component/Animation/AnimationComponent.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Function/Framework/Level/Level.h"
#include "Runtime/Function/Timeline/TimelineEvaluation.h"

#include <algorithm>
#include <cmath>
#include <vector>

TimelineDirector::TimelineDirector() {}

TimelineDirector::~TimelineDirector()
{
    // Only voices are released here. Restoring activation / animation would touch
    // GameObjects through m_Level, and a director usually dies during level teardown,
    // when neither is guaranteed to be alive any more. A voice left running would
    // play attached to nothing and hold one of the mixer's voice slots.
    if (AudioSystem* audio_system = GET_SYSTEM(AudioSystem))
    {
        for (auto& [clip, runtime] : m_ClipRuntime)
        {
            if (runtime.voice != kInvalidAudioVoiceId)
            {
                audio_system->Stop(runtime.voice);
            }
        }
    }
}

bool TimelineDirector::Initialize(TimelineAsset* timeline_asset, GameObject* owner_object, Level* level)
{
    if (!timeline_asset || !owner_object || !level)
    {
        return false;
    }

    // Re-initialising must not leak whatever the previous asset had applied.
    if (m_Initialized)
    {
        ExitAllClips();
    }

    m_TimelineAsset = timeline_asset;
    m_OwnerObject = owner_object;
    m_Level = level;
    m_Initialized = true;

    m_CurrentTime = 0.0f;
    m_PlayState = TimelinePlayState::Stopped;
    m_ClipRuntime.clear();

    return true;
}

void TimelineDirector::Tick(float delta_time)
{
    if (!m_Initialized || !m_TimelineAsset || m_PlayState != TimelinePlayState::Playing)
    {
        return;
    }

    const TimelineEval::TimeStep step =
        TimelineEval::Advance(m_CurrentTime, delta_time, m_Speed, m_TimelineAsset->m_Duration, m_Loop);
    m_CurrentTime = step.new_time;

    // Evaluate BEFORE acting on `finished`: the last stretch still fires its events
    // (an event at the very end included) and exits the clips that end there.
    Evaluate(&step);

    if (step.finished)
    {
        // Hold the final frame (time stays at the end) rather than snapping back to 0
        // and re-applying whatever clips start at 0 -- the old Stop()-at-end did that.
        // Play() from here restarts from the beginning.
        PauseAudio(true);
        m_PlayState = TimelinePlayState::Stopped;
    }
}

void TimelineDirector::Play()
{
    if (!m_Initialized || !m_TimelineAsset)
    {
        return;
    }

    if (m_PlayState == TimelinePlayState::Stopped)
    {
        // Finished at an end: start over from the end playback moves away from.
        const float duration = m_TimelineAsset->m_Duration;
        const bool at_forward_end = m_Speed >= 0.0f && m_CurrentTime >= duration;
        const bool at_reverse_end = m_Speed < 0.0f && m_CurrentTime <= 0.0f;
        if (at_forward_end || at_reverse_end)
        {
            ExitAllClips();
            m_CurrentTime = m_Speed >= 0.0f ? 0.0f : std::max(duration, 0.0f);
            Evaluate(nullptr);
        }
    }

    const bool resuming = m_PlayState == TimelinePlayState::Paused;
    m_PlayState = TimelinePlayState::Playing;
    if (resuming)
    {
        PauseAudio(false);
    }
}

void TimelineDirector::Pause()
{
    if (!m_Initialized || m_PlayState != TimelinePlayState::Playing)
    {
        return;
    }

    m_PlayState = TimelinePlayState::Paused;
    PauseAudio(true);
}

void TimelineDirector::Stop()
{
    if (!m_Initialized)
    {
        return;
    }

    ExitAllClips();
    m_PlayState = TimelinePlayState::Stopped;
    m_CurrentTime = 0.0f;
}

void TimelineDirector::SetTime(float time)
{
    if (!m_Initialized || !m_TimelineAsset)
    {
        return;
    }

    const float duration = std::max(m_TimelineAsset->m_Duration, 0.0f);
    m_CurrentTime = std::clamp(std::isfinite(time) ? time : 0.0f, 0.0f, duration);
    // A jump: clip windows are re-evaluated (enter / exit), events do not fire.
    Evaluate(nullptr);
}

void TimelineDirector::Evaluate(const TimelineEval::TimeStep* step)
{
    if (!m_TimelineAsset)
    {
        return;
    }

    for (auto& track_ptr : m_TimelineAsset->m_Tracks)
    {
        TimelineTrack* track = track_ptr;
        if (track == nullptr)
        {
            continue;
        }

        // A muted / disabled track still has to EXIT clips that were active before it
        // was muted, so its clips are visited with "not live" instead of skipped.
        const bool track_live = track->m_Enabled && !track->m_Muted;
        for (auto& clip_ptr : track->m_Clips)
        {
            TimelineClip* clip = clip_ptr;
            if (clip != nullptr)
            {
                EvaluateClip(track, clip, track_live, step);
            }
        }
    }
}

void TimelineDirector::EvaluateClip(TimelineTrack* track,
                                    TimelineClip* clip,
                                    bool track_live,
                                    const TimelineEval::TimeStep* step)
{
    // Event clips are points, not windows: they fire when playback crosses their start.
    if (auto* event_clip = Cast<EventTimelineClip>(clip))
    {
        if (track_live && clip->m_Enabled && step != nullptr && step->Crosses(clip->m_StartTime))
        {
            FireEvent(event_clip, track->m_TargetObjectId);
        }
        return;
    }

    ClipKind kind;
    uint64_t target_id = track->m_TargetObjectId;
    if (Cast<AnimationTimelineClip>(clip) != nullptr)
    {
        kind = ClipKind::Animation;
    }
    else if (auto* activation_clip = Cast<ActivationTimelineClip>(clip))
    {
        kind = ClipKind::Activation;
        // The clip carries its own target; it wins over the track's when set.
        if (activation_clip->m_TargetObjectId != 0)
        {
            target_id = activation_clip->m_TargetObjectId;
        }
    }
    else if (Cast<AudioTimelineClip>(clip) != nullptr)
    {
        kind = ClipKind::Audio;
    }
    else
    {
        return;
    }

    const bool now_active =
        track_live && clip->m_Enabled && TimelineEval::ClipActiveAt(clip->m_StartTime, clip->m_Duration, m_CurrentTime);

    auto found = m_ClipRuntime.find(clip);
    const bool was_active = found != m_ClipRuntime.end() && found->second.active;

    switch (TimelineEval::Transition(was_active, now_active))
    {
        case TimelineEval::ClipTransition::None:
            break;
        case TimelineEval::ClipTransition::Enter:
        {
            ClipRuntime& runtime = m_ClipRuntime[clip];
            runtime = ClipRuntime {};
            runtime.kind = kind;
            runtime.target_id = target_id;
            runtime.active = true;
            EnterClip(clip, runtime);
            StayClip(clip, runtime);
            break;
        }
        case TimelineEval::ClipTransition::Stay:
            StayClip(clip, found->second);
            break;
        case TimelineEval::ClipTransition::Exit:
            ExitClip(found->second);
            break;
    }
}

void TimelineDirector::EnterClip(TimelineClip* clip, ClipRuntime& runtime)
{
    GameObject* target = GetTargetObject(runtime.target_id);
    if (target == nullptr)
    {
        return;
    }

    switch (runtime.kind)
    {
        case ClipKind::Activation:
        {
            auto* activation_clip = static_cast<ActivationTimelineClip*>(clip);
            if (target == m_OwnerObject && !activation_clip->m_Active)
            {
                // A deactivated object stops ticking (GameObject::Tick), and this
                // director ticks from it: it would freeze and never reach the clip's
                // exit to reactivate itself.
                if (!m_OwnerDeactivationWarned)
                {
                    m_OwnerDeactivationWarned = true;
                    LOG_WARNING(ZGame,
                                "TimelineDirector: an activation clip would deactivate the Timeline's own object "
                                "'{}'; ignored (it would stop the Timeline itself)",
                                target->GetName().c_str());
                }
                return;
            }
            Transform* transform = target->tryGetComponent(Transform);
            if (transform == nullptr)
            {
                return;
            }
            runtime.saved_active = transform->isEnabled();
            // 走 GameObject::SetActive 而不是直接 transform->SetEnabled：后者只写
            // flag，不会通知组件，网格会留在画面上（GameObject::Tick 停用后直接
            // return，BaseRenderer 再没机会撤实体）。SetActive 是唯一入口。
            target->SetActive(activation_clip->m_Active);
            runtime.applied = true;
            break;
        }
        case ClipKind::Animation:
        {
            auto* animation_clip = static_cast<AnimationTimelineClip*>(clip);
            auto* component = target->tryGetComponent<AnimationComponent>("AnimationComponent");
            const bool same_clip = component != nullptr && component->GetClipPath() == animation_clip->m_AnimationPath;
            runtime.animation_length = TimelineEval::ResolveAnimationLength(
                same_clip, same_clip ? component->GetClipLength() : 0.0f, clip->m_Duration, animation_clip->m_Speed);
            break;
        }
        case ClipKind::Audio:
            // Started in StayClip(), which also covers "entered while paused, then played".
            break;
    }
}

void TimelineDirector::StayClip(TimelineClip* clip, ClipRuntime& runtime)
{
    switch (runtime.kind)
    {
        case ClipKind::Animation:
            UpdateAnimation(static_cast<AnimationTimelineClip*>(clip), runtime);
            break;
        case ClipKind::Audio:
            StartAudioIfNeeded(static_cast<AudioTimelineClip*>(clip), runtime);
            break;
        case ClipKind::Activation:
            break;
    }
}

void TimelineDirector::ExitClip(ClipRuntime& runtime)
{
    runtime.active = false;

    switch (runtime.kind)
    {
        case ClipKind::Audio:
            if (runtime.voice != kInvalidAudioVoiceId)
            {
                if (AudioSystem* audio_system = GET_SYSTEM(AudioSystem))
                {
                    audio_system->Stop(runtime.voice);
                }
                runtime.voice = kInvalidAudioVoiceId;
            }
            runtime.audio_attempted = false;
            break;
        case ClipKind::Activation:
            if (runtime.applied)
            {
                runtime.applied = false;
                if (GameObject* target = GetTargetObject(runtime.target_id))
                {
                    // 同 EnterClip：走 SetActive，别直接写 Transform 的 flag。
                    target->SetActive(runtime.saved_active);
                }
            }
            break;
        case ClipKind::Animation:
            if (GameObject* target = GetTargetObject(runtime.target_id))
            {
                if (auto* component = target->tryGetComponent<AnimationComponent>("AnimationComponent"))
                {
                    component->ClearTimelineOverride();
                }
            }
            break;
    }
}

void TimelineDirector::ExitAllClips()
{
    // Reverse authoring order is not tracked; activation clips overlapping on the same
    // target restore in map order, which only matters for overlapping clips on one
    // object (an authoring conflict in any case).
    for (auto& [clip, runtime] : m_ClipRuntime)
    {
        if (runtime.active)
        {
            ExitClip(runtime);
        }
    }
}

void TimelineDirector::UpdateAnimation(AnimationTimelineClip* clip, ClipRuntime& runtime)
{
    GameObject* target = GetTargetObject(runtime.target_id);
    if (target == nullptr)
    {
        return;
    }
    auto* component = target->tryGetComponent<AnimationComponent>("AnimationComponent");
    if (component == nullptr)
    {
        return;
    }

    const float local_time = m_CurrentTime - clip->m_StartTime;
    const float phase = TimelineEval::AnimationPhase(local_time, clip->m_Speed, runtime.animation_length, clip->m_Loop);
    if (!component->SetTimelineOverride(clip->m_AnimationPath, phase) && !m_AnimationOverrideWarned)
    {
        m_AnimationOverrideWarned = true;
        LOG_WARNING(ZAnimation,
                    "TimelineDirector: cannot drive '{}' on '{}' (empty clip path, or the AnimationComponent has "
                    "no bone map to sample through)",
                    clip->m_AnimationPath.c_str(),
                    target->GetName().c_str());
    }
}

void TimelineDirector::StartAudioIfNeeded(AudioTimelineClip* clip, ClipRuntime& runtime)
{
    // Only while playing: a scrub (SetTime while paused / stopped) must not start sound.
    // One attempt per entry, so a missing file is not retried (and logged) every frame.
    if (m_PlayState != TimelinePlayState::Playing || runtime.audio_attempted)
    {
        return;
    }
    runtime.audio_attempted = true;

    AudioSystem* audio_system = GET_SYSTEM(AudioSystem);
    if (audio_system == nullptr || clip->m_AudioPath.empty())
    {
        return;
    }

    AudioPlayParams params;
    params.m_Volume = std::clamp(clip->m_Volume, 0.0f, 1.0f);
    params.m_Loop = clip->m_Loop;
    params.m_SpatialBlend = 0.0f;  // Timeline audio is 2D, like Unity's AudioTrack without a source.
    runtime.voice = audio_system->PlayOneShot(clip->m_AudioPath.c_str(), params);
}

void TimelineDirector::PauseAudio(bool paused)
{
    AudioSystem* audio_system = GET_SYSTEM(AudioSystem);
    if (audio_system == nullptr)
    {
        return;
    }
    for (auto& [clip, runtime] : m_ClipRuntime)
    {
        if (runtime.active && runtime.voice != kInvalidAudioVoiceId)
        {
            audio_system->SetPaused(runtime.voice, paused);
        }
    }
}

void TimelineDirector::FireEvent(EventTimelineClip* clip, uint64_t target_id)
{
    TimelineEventInfo info;
    info.name.assign(clip->m_EventName.c_str(), clip->m_EventName.size());
    info.params.assign(clip->m_EventParams.c_str(), clip->m_EventParams.size());
    info.time = clip->m_StartTime;
    info.target = GetTargetObject(target_id);

    if (m_EventHandler)
    {
        m_EventHandler(info);
    }
    else
    {
        LOG_DEBUG(ZGame, "Timeline event '{}' at {:.3f}s params={}", info.name, info.time, info.params);
    }
}

GameObject* TimelineDirector::GetTargetObject(uint64_t object_id)
{
    if (object_id == 0)
    {
        return m_OwnerObject;
    }

    if (!m_Level)
    {
        return nullptr;
    }

    auto go_weak = m_Level->GetGObjectByID(static_cast<GObjectID>(object_id));
    if (auto go_shared = go_weak.lock())
    {
        return go_shared.get();
    }

    return nullptr;
}
