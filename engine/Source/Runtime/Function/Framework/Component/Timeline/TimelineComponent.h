#pragma once

#include "Runtime/Core/Serialize/SerializeUtility.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Function/Timeline/TimelineDirector.h"
#include "Runtime/Resource/ResType/Components/Timeline.h"

#include <memory>

class TimelineAsset;

// Timeline组件（附加到GameObject上）
class TimelineComponent : public Component
{
public:
    TimelineComponent() = default;
    ~TimelineComponent() override = default;

    void PostLoadResource(GameObject* parent_object) override;
    void Tick(float delta_time) override;

    // 播放Timeline
    void Play();

    // 暂停Timeline
    void Pause();

    // 停止Timeline
    void Stop();

    // 设置播放时间
    void SetTime(float time);

    // 获取当前播放时间
    float GetTime() const;

    // 获取播放状态
    TimelinePlayState GetPlayState() const;

    // 设置播放速度
    void SetSpeed(float speed);
    float GetSpeed() const;

    // 设置是否循环
    void SetLoop(bool loop);
    bool GetLoop() const;

    // 获取Timeline资源
    TimelineAsset* GetTimelineAsset() const;

protected:
    TimelineComponentRes m_TimelineRes;

private:
    // 加载Timeline资源
    bool LoadTimelineAsset();

    std::shared_ptr<TimelineDirector> m_Director;
    TimelineAsset* m_TimelineAsset;
    bool m_AssetLoaded {false};
};