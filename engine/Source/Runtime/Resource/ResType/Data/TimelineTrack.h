#pragma once

#include "Runtime/Core/Serialize/SerializeUtility.h"
#include "Runtime/Resource/ResType/Data/TimelineClip.h"

#include <string>
#include <vector>

// Timeline轨道基类
class TimelineTrack : public Object
{
    REGISTER_CLASS(TimelineTrack)

public:
    DECLARE_SERIALIZE(TimelineTrack)

    TimelineTrack() = default;
    virtual ~TimelineTrack() = default;

    // 轨道名称
    std::string m_Name;

    // 轨道是否启用
    bool m_Enabled {true};

    // 轨道是否锁定（编辑器中不可编辑）
    bool m_Locked {false};

    // 轨道是否静音
    bool m_Muted {false};

    // 目标GameObject的ID（0表示当前GameObject）
    uint64_t m_TargetObjectId {0};

    // 轨道上的所有片段
    std::vector<PPtr<TimelineClip>> m_Clips;
};

// 动画轨道
class AnimationTimelineTrack : public TimelineTrack
{
public:
    AnimationTimelineTrack() = default;
};

// 激活轨道
class ActivationTimelineTrack : public TimelineTrack
{
public:
    ActivationTimelineTrack() = default;
};

// 音频轨道
class AudioTimelineTrack : public TimelineTrack
{
public:
    AudioTimelineTrack() = default;
};

// 事件轨道
class EventTimelineTrack : public TimelineTrack
{
public:
    EventTimelineTrack() = default;
};