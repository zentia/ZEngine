#pragma once

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Resource/ResType/Data/TimelineAsset.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

class GameObject;
class Level;

namespace TimelineEval
{
    struct TimeStep;
}

// Timeline播放状态
enum class TimelinePlayState
{
    Stopped,
    Playing,
    Paused
};

/// What an event clip hands to the handler when the playhead passes its start time.
struct TimelineEventInfo
{
    std::string name;
    /// The clip's free-form parameter string (authored as JSON; passed through verbatim).
    std::string params;
    /// Timeline time the event is authored at (the clip's start).
    float time {0.0f};
    /// The event track's target (the Timeline's owner when the track has no target).
    GameObject* target {nullptr};
};

using TimelineEventHandler = std::function<void(const TimelineEventInfo&)>;

// Timeline导演（管理Timeline的播放）
//
// Clip semantics (each clip is evaluated as enter / stay / exit against its window
// [start, start + duration); the pure rules live in TimelineEvaluation.h and are
// tested headless):
// - Animation: while active, the target's AnimationComponent samples the clip at the
//   phase given by the local time (transient override, nothing serialized); on exit
//   the component resumes its own playback.
// - Activation: on enter the target's active flag (Transform::m_Enabled) is set to the
//   clip's value; on exit it is restored to what it was on enter. The Timeline's own
//   object is never deactivated -- a deactivated object stops ticking, which would
//   freeze the Timeline with no way to resume.
// - Audio: a 2D voice is started on enter while playing, paused/resumed with the
//   Timeline, stopped on exit or Stop(). Starts from the beginning of the sound (the
//   audio system has no seek), so scrubbing into the middle of an audio clip does not
//   play it until the Timeline is playing.
// - Event: fires once each time playback moves the playhead past the clip's start
//   (a point, so a long frame cannot skip it; loops fire it again each cycle).
//   SetTime() jumps and does not fire events.
class TimelineDirector
{
public:
    TimelineDirector();
    ~TimelineDirector();

    // 初始化Timeline
    bool Initialize(TimelineAsset* timeline_asset, GameObject* owner_object, Level* level);

    // 更新Timeline（每帧调用）
    void Tick(float delta_time);

    // 播放（从停止状态的末尾播放时从头开始）
    void Play();

    // 暂停
    void Pause();

    // 停止：退出所有片段（恢复激活状态、停止音频、交还动画），时间回到 0
    void Stop();

    // 设置播放时间（跳转，不触发事件）
    void SetTime(float time);

    // 获取当前播放时间
    float GetTime() const { return m_CurrentTime; }

    // 获取播放状态
    TimelinePlayState GetPlayState() const { return m_PlayState; }

    // 设置播放速度（负数倒放）
    void SetSpeed(float speed) { m_Speed = speed; }
    float GetSpeed() const { return m_Speed; }

    // 设置是否循环
    void SetLoop(bool loop) { m_Loop = loop; }
    bool GetLoop() const { return m_Loop; }

    /// Receives every event clip that fires. Without a handler events are only logged.
    void SetEventHandler(TimelineEventHandler handler) { m_EventHandler = std::move(handler); }

    // 获取Timeline资源
    TimelineAsset* GetTimelineAsset() const { return m_TimelineAsset; }

    // 是否已初始化
    bool isInitialized() const { return m_Initialized; }

private:
    enum class ClipKind : uint8_t
    {
        Animation,
        Activation,
        Audio
    };

    /// Per-clip runtime state, keyed by clip. Holds object IDs, never pointers: a target
    /// destroyed while its clip is active must not be touched on exit.
    struct ClipRuntime
    {
        ClipKind kind {ClipKind::Animation};
        bool active {false};
        /// Resolved target (0 = the Timeline's owner).
        uint64_t target_id {0};
        // Activation
        bool applied {false};
        bool saved_active {true};
        // Animation
        float animation_length {0.0f};
        // Audio
        uint64_t voice {0};
        bool audio_attempted {false};
    };

    /// Evaluates every clip at m_CurrentTime. `step` is the playback stretch of this
    /// tick (event clips fire only for times it crosses); null for SetTime() jumps.
    void Evaluate(const TimelineEval::TimeStep* step);
    void EvaluateClip(TimelineTrack* track, TimelineClip* clip, bool track_live, const TimelineEval::TimeStep* step);

    void EnterClip(TimelineClip* clip, ClipRuntime& runtime);
    void StayClip(TimelineClip* clip, ClipRuntime& runtime);
    void ExitClip(ClipRuntime& runtime);
    void ExitAllClips();

    void UpdateAnimation(AnimationTimelineClip* clip, ClipRuntime& runtime);
    void StartAudioIfNeeded(AudioTimelineClip* clip, ClipRuntime& runtime);
    void PauseAudio(bool paused);
    void FireEvent(EventTimelineClip* clip, uint64_t target_id);

    // 获取目标GameObject（0 = owner）
    GameObject* GetTargetObject(uint64_t object_id);

    TimelineAsset* m_TimelineAsset {nullptr};
    GameObject* m_OwnerObject {nullptr};
    Level* m_Level {nullptr};

    TimelinePlayState m_PlayState {TimelinePlayState::Stopped};
    float m_CurrentTime {0.0f};
    float m_Speed {1.0f};
    bool m_Loop {false};
    bool m_Initialized {false};

    std::unordered_map<const TimelineClip*, ClipRuntime> m_ClipRuntime;
    TimelineEventHandler m_EventHandler;
    bool m_OwnerDeactivationWarned {false};
    bool m_AnimationOverrideWarned {false};
};
