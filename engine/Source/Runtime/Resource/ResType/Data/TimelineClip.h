#pragma once

#include "Runtime/Core/Serialize/SerializeUtility.h"

#include <EASTL/string.h>
#include "TimelineClip.generated.h"

// Timeline 片段基类。片段是 TimelineAsset 文件里的子对象（Unity .playable
// 布局：一个文件 = TimelineAsset@1 + 各轨道 + 各片段），由 TimelineTrack::m_Clips
// 的 PPtr 引用；整体读写走 TimelineAssetIO（对象图），不要用单对象的 loadAsset。
// 字符串字段用 eastl::string：SerializeTraits 只特化了 eastl::string。
ZCLASS(Abstract)
class TimelineClip : public Object
{
    ZGENERATED_BODY(TimelineClip)

public:
    TimelineClip() = default;
    ~TimelineClip() override = default;

    // 片段的开始时间（相对于轨道）
    ZPROPERTY()
    float m_StartTime {0.0f};

    // 片段的持续时间
    ZPROPERTY()
    float m_Duration {1.0f};

    // 片段是否启用
    ZPROPERTY()
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
ZCLASS()
class AnimationTimelineClip : public TimelineClip
{
    ZGENERATED_BODY(AnimationTimelineClip)

public:
    AnimationTimelineClip() = default;

    // 动画资源路径
    ZPROPERTY()
    eastl::string m_AnimationPath;

    // 播放速度倍率
    ZPROPERTY()
    float m_Speed {1.0f};

    // 是否循环播放
    ZPROPERTY()
    bool m_Loop {false};
};

// 激活片段（控制GameObject的激活状态）
ZCLASS()
class ActivationTimelineClip : public TimelineClip
{
    ZGENERATED_BODY(ActivationTimelineClip)

public:
    ActivationTimelineClip() = default;

    // 目标GameObject的ID（0表示当前GameObject）
    ZPROPERTY()
    uint64_t m_TargetObjectId {0};

    // 激活状态
    ZPROPERTY()
    bool m_Active {true};
};

// 音频片段
ZCLASS()
class AudioTimelineClip : public TimelineClip
{
    ZGENERATED_BODY(AudioTimelineClip)

public:
    AudioTimelineClip() = default;

    // 音频资源路径
    ZPROPERTY()
    eastl::string m_AudioPath;

    // 音量（0-1）
    ZPROPERTY()
    float m_Volume {1.0f};

    // 是否循环
    ZPROPERTY()
    bool m_Loop {false};
};

// 事件片段（触发自定义事件）
ZCLASS()
class EventTimelineClip : public TimelineClip
{
    ZGENERATED_BODY(EventTimelineClip)

public:
    EventTimelineClip() = default;

    // 事件名称
    ZPROPERTY()
    eastl::string m_EventName;

    // 事件参数（JSON格式字符串）
    ZPROPERTY()
    eastl::string m_EventParams;
};
