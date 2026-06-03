#pragma once

#include "Runtime/Core/Serialize/SerializeUtility.h"
#include "Runtime/Resource/ResType/Data/TimelineAsset.h"

#include <string>

// Timeline组件资源
class TimelineComponentRes
{
public:
    DECLARE_SERIALIZE(TimelineComponentRes)

    // Timeline资源路径
    eastl::string m_TimelineAssetPath;

    // 是否在开始时自动播放
    bool m_PlayOnAwake {false};

    // 是否循环播放
    bool m_Loop {false};

    // 播放速度倍率
    float m_Speed {1.0f};

    // 初始播放时间（秒）
    float m_InitialTime {0.0f};
};