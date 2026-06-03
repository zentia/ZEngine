#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/BaseClasses/PPtr.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"
#include "Runtime/Resource/ResType/Data/TimelineTrack.h"

#include <string>
#include <vector>

// Timeline资源（类似Unity的Timeline Asset）
class TimelineAsset : public Object
{
    REGISTER_CLASS(TimelineAsset);
    DECLARE_OBJECT_SERIALIZE();

public:
    TimelineAsset() = default;

    // Timeline总时长（秒）
    float m_Duration {10.0f};

    // 帧率（用于时间轴显示和编辑）
    float m_FrameRate {30.0f};

    // 所有轨道
    std::vector<PPtr<TimelineTrack>> m_Tracks;

    // 获取时间对应的帧数
    int getFrameAtTime(float time) const { return static_cast<int>(time * m_FrameRate); }

    // 获取帧数对应的时间
    float getTimeAtFrame(int frame) const { return static_cast<float>(frame) / m_FrameRate; }

    // 获取实际的总时长（基于所有轨道和片段）
    float calculateActualDuration() const
    {
        float max_duration = 0.0f;
        for (const auto& track : m_Tracks)
        {
            if (!track || !track->m_Enabled)
                continue;

            for (const auto& clip : track->m_Clips)
            {
                if (!clip || !clip->m_Enabled)
                    continue;

                float clip_end = clip->getEndTime();
                if (clip_end > max_duration)
                    max_duration = clip_end;
            }
        }
        return max_duration > 0.0f ? max_duration : m_Duration;
    }
};