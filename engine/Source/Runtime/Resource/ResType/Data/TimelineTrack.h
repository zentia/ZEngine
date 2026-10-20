#pragma once

#include "Runtime/BaseClasses/PPtr.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"
#include "Runtime/Resource/ResType/Data/TimelineClip.h"

#include <EASTL/string.h>
#include <vector>
#include "TimelineTrack.generated.h"

// Timeline 轨道基类。轨道是 TimelineAsset 文件里的子对象，由
// TimelineAsset::m_Tracks 的 PPtr 引用；片段同理挂在 m_Clips 上。
// 新建轨道/片段必须拿到 InstanceID（ObjectManager::NewObject<T>()），
// 否则 PPtr 构造出来就是空引用。
ZCLASS(Abstract)
class TimelineTrack : public Object
{
    ZGENERATED_BODY(TimelineTrack)

public:
    TimelineTrack() = default;
    ~TimelineTrack() override = default;

    // 轨道名称
    ZPROPERTY()
    eastl::string m_Name;

    // 轨道是否启用
    ZPROPERTY()
    bool m_Enabled {true};

    // 轨道是否锁定（编辑器中不可编辑）
    ZPROPERTY()
    bool m_Locked {false};

    // 轨道是否静音
    ZPROPERTY()
    bool m_Muted {false};

    // 目标GameObject的ID（0表示当前GameObject）
    ZPROPERTY()
    uint64_t m_TargetObjectId {0};

    // 轨道上的所有片段
    ZPROPERTY()
    std::vector<PPtr<TimelineClip>> m_Clips;
};

// 动画轨道
ZCLASS()
class AnimationTimelineTrack : public TimelineTrack
{
    ZGENERATED_BODY(AnimationTimelineTrack)

public:
    AnimationTimelineTrack() = default;
};

// 激活轨道
ZCLASS()
class ActivationTimelineTrack : public TimelineTrack
{
    ZGENERATED_BODY(ActivationTimelineTrack)

public:
    ActivationTimelineTrack() = default;
};

// 音频轨道
ZCLASS()
class AudioTimelineTrack : public TimelineTrack
{
    ZGENERATED_BODY(AudioTimelineTrack)

public:
    AudioTimelineTrack() = default;
};

// 事件轨道
ZCLASS()
class EventTimelineTrack : public TimelineTrack
{
    ZGENERATED_BODY(EventTimelineTrack)

public:
    EventTimelineTrack() = default;
};
