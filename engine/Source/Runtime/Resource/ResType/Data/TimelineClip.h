#pragma once

#include "Runtime/Core/Serialize/SerializeUtility.h"

#include <string>

// Timeline片段基类
class TimelineClip : public Object
{
    REGISTER_CLASS(TimelineClip)

public:
    TimelineClip() = default;
    virtual ~TimelineClip() = default;

    // 片段的开始时间（相对于轨道）
    float m_StartTime {0.0f};

    // 片段的持续时间
    float m_Duration {1.0f};

    // 片段是否启用
    bool m_Enabled {true};

    // 获取片段的结束时间
    float getEndTime() const { return m_StartTime + m_Duration; }

    // 检查指定时间是否在片段范围内
    bool containsTime(float time) const { return m_Enabled && time >= m_StartTime && time < getEndTime(); }

    // 获取片段内的归一化时间（0-1）
    float getNormalizedTime(float global_time) const
    {
        if (m_Duration <= 0.0f)
            return 0.0f;
        return (global_time - m_StartTime) / m_Duration;
    }
};

// 动画片段
class AnimationTimelineClip : public TimelineClip
{
    DECLARE_SERIALIZE(AnimationTimelineClip)

public:
    AnimationTimelineClip() = default;

    // 动画资源路径
    std::string m_AnimationPath;

    // 播放速度倍率
    float m_Speed {1.0f};

    // 是否循环播放
    bool m_Loop {false};
};

// 激活片段（控制GameObject的激活状态）
class ActivationTimelineClip : public TimelineClip
{
public:
    ActivationTimelineClip() = default;

    // 目标GameObject的ID（0表示当前GameObject）
    uint64_t m_TargetObjectId {0};

    // 激活状态
    bool m_Active {true};
};

// 音频片段
class AudioTimelineClip : public TimelineClip
{
    DECLARE_SERIALIZE(AudioTimelineClip)

public:
    AudioTimelineClip() = default;

    // 音频资源路径
    std::string m_AudioPath;

    // 音量（0-1）
    float m_Volume {1.0f};

    // 是否循环
    bool m_Loop {false};
};

// 事件片段（触发自定义事件）
class EventTimelineClip : public TimelineClip
{
public:
    DECLARE_SERIALIZE(EventTimelineClip)

    EventTimelineClip() = default;

    // 事件名称
    std::string m_EventName;

    // 事件参数（JSON格式字符串）
    std::string m_EventParams;
};