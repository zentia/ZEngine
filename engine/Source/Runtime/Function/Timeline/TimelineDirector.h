#pragma once

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Resource/ResType/Data/TimelineAsset.h"

#include <memory>
#include <unordered_map>

class GameObject;
class Level;

// Timeline播放状态
enum class TimelinePlayState
{
    Stopped,
    Playing,
    Paused
};

// Timeline导演（管理Timeline的播放）
class TimelineDirector
{
public:
    TimelineDirector();
    ~TimelineDirector();

    // 初始化Timeline
    bool Initialize(TimelineAsset* timeline_asset, GameObject* owner_object, Level* level);

    // 更新Timeline（每帧调用）
    void Tick(float delta_time);

    // 播放
    void Play();

    // 暂停
    void Pause();

    // 停止
    void Stop();

    // 设置播放时间
    void SetTime(float time);

    // 获取当前播放时间
    float GetTime() const { return m_CurrentTime; }

    // 获取播放状态
    TimelinePlayState GetPlayState() const { return m_PlayState; }

    // 设置播放速度
    void SetSpeed(float speed) { m_Speed = speed; }
    float GetSpeed() const { return m_Speed; }

    // 设置是否循环
    void SetLoop(bool loop) { m_Loop = loop; }
    bool GetLoop() const { return m_Loop; }

    // 获取Timeline资源
    TimelineAsset* GetTimelineAsset() const { return m_TimelineAsset; }

    // 是否已初始化
    bool isInitialized() const { return m_Initialized; }

private:
    // 更新所有轨道
    void UpdateTracks(float time);

    // 更新单个轨道
    void UpdateTrack(TimelineTrack* track, float time);

    // 更新动画片段
    void UpdateAnimationClip(AnimationTimelineClip* clip, float normalized_time, GameObject* target_object);

    // 更新激活片段
    void UpdateActivationClip(ActivationTimelineClip* clip, GameObject* target_object);

    // 更新音频片段
    void UpdateAudioClip(AudioTimelineClip* clip, float normalized_time, GameObject* target_object);

    // 更新事件片段
    void UpdateEventClip(EventTimelineClip* clip, GameObject* target_object);

    // 获取目标GameObject
    GameObject* GetTargetObject(uint64_t object_id);

    TimelineAsset* m_TimelineAsset;
    GameObject* m_OwnerObject {nullptr};
    Level* m_Level {nullptr};

    TimelinePlayState m_PlayState {TimelinePlayState::Stopped};
    float m_CurrentTime {0.0f};
    float m_Speed {1.0f};
    bool m_Loop {false};
    bool m_Initialized {false};

    // 用于跟踪已触发的事件（避免重复触发）
    std::unordered_map<EventTimelineClip*, bool> m_EventTriggeredMap;
};