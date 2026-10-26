#pragma once

#include "Runtime/Core/Serialize/SerializeUtility.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Function/Timeline/TimelineDirector.h"
#include "Runtime/Resource/ResType/Components/Timeline.h"

#include <memory>
#include "TimelineComponent.generated.h"

class TimelineAsset;

// Timeline组件（附加到GameObject上）
ZCLASS()
class TimelineComponent : public Component
{
    ZGENERATED_BODY(TimelineComponent)

public:
    TimelineComponent() = default;
    ~TimelineComponent() override = default;

    void PostLoadResource(GameObject* parent_object) override;
    void Tick(float delta_time) override;

    // ---- 播放控制（ZFUNCTION：过场脚本的唯一入口）----
    // 注意这些都只作用于 director：没加载出 TimelineAsset 时是空操作，不会崩。
    ZFUNCTION()
    void Play();

    ZFUNCTION()
    void Pause();

    ZFUNCTION()
    void Stop();

    ZFUNCTION()
    void SetTime(float time);

    ZFUNCTION()
    float GetTime() const;

    /// TimelinePlayState（Stopped / Playing / Paused）。
    ZFUNCTION()
    TimelinePlayState GetPlayState() const;

    ZFUNCTION()
    void SetSpeed(float speed);
    ZFUNCTION()
    float GetSpeed() const;

    ZFUNCTION()
    void SetLoop(bool loop);
    ZFUNCTION()
    bool GetLoop() const;

    /// 不导出：TimelineAsset 不是 Object 派生类，`T*` 的 Converter 要求
    /// `std::is_base_of<Object, T>`，标了会直接编译失败。脚本也不需要摸资产本身。
    TimelineAsset* GetTimelineAsset() const;

    /// 不导出：TimelineEventHandler 是 std::function，跨语言没有等价物。
    /// 过场要通知脚本，请在 Timeline 里放一条事件轨道并让脚本轮询，或由 C++ 侧转接。
    void SetEventHandler(TimelineEventHandler handler);

protected:
    ZPROPERTY(Name="timeline_res")
    TimelineComponentRes m_TimelineRes;

private:
    // 加载Timeline资源
    bool LoadTimelineAsset();

    std::shared_ptr<TimelineDirector> m_Director;
    TimelineEventHandler m_EventHandler;
    TimelineAsset* m_TimelineAsset {nullptr};
    bool m_AssetLoaded {false};
};