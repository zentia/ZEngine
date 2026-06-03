#include "TimelineComponent.h"

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Function/Framework/Level/Level.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/ResType/Data/TimelineAsset.h"

void TimelineComponent::PostLoadResource(GameObject* parent_object)
{
    Component::PostLoadResource(parent_object);

    // 创建Director
    m_Director = std::make_shared<TimelineDirector>();

    // 加载Timeline资源
    if (LoadTimelineAsset())
    {
        // 获取Level（通过WorldManager）
        Level* level = nullptr;
        auto level_weak = GET_SYSTEM(WorldManager)->getCurrentActiveLevel();
        if (auto level_shared = level_weak)
        {
            level = level_shared;
        }

        // 初始化Director
        if (m_Director->Initialize(m_TimelineAsset, parent_object, level))
        {
            // 设置初始参数
            m_Director->SetSpeed(m_TimelineRes.m_Speed);
            m_Director->SetLoop(m_TimelineRes.m_Loop);
            m_Director->SetTime(m_TimelineRes.m_InitialTime);

            // 如果设置了自动播放
            if (m_TimelineRes.m_PlayOnAwake)
            {
                m_Director->Play();
            }
        }
    }
}

void TimelineComponent::Tick(float delta_time)
{
    Component::Tick(delta_time);

    if (m_Director && m_Director->isInitialized())
    {
        m_Director->Tick(delta_time);
    }
}

void TimelineComponent::Play()
{
    if (m_Director)
    {
        m_Director->Play();
    }
}

void TimelineComponent::Pause()
{
    if (m_Director)
    {
        m_Director->Pause();
    }
}

void TimelineComponent::Stop()
{
    if (m_Director)
    {
        m_Director->Stop();
    }
}

void TimelineComponent::SetTime(float time)
{
    if (m_Director)
    {
        m_Director->SetTime(time);
    }
}

float TimelineComponent::GetTime() const
{
    if (m_Director)
    {
        return m_Director->GetTime();
    }
    return 0.0f;
}

TimelinePlayState TimelineComponent::GetPlayState() const
{
    if (m_Director)
    {
        return m_Director->GetPlayState();
    }
    return TimelinePlayState::Stopped;
}

void TimelineComponent::SetSpeed(float speed)
{
    if (m_Director)
    {
        m_Director->SetSpeed(speed);
    }
    m_TimelineRes.m_Speed = speed;
}

float TimelineComponent::GetSpeed() const
{
    if (m_Director)
    {
        return m_Director->GetSpeed();
    }
    return m_TimelineRes.m_Speed;
}

void TimelineComponent::SetLoop(bool loop)
{
    if (m_Director)
    {
        m_Director->SetLoop(loop);
    }
    m_TimelineRes.m_Loop = loop;
}

bool TimelineComponent::GetLoop() const
{
    if (m_Director)
    {
        return m_Director->GetLoop();
    }
    return m_TimelineRes.m_Loop;
}

TimelineAsset* TimelineComponent::GetTimelineAsset() const
{
    return m_TimelineAsset;
}

bool TimelineComponent::LoadTimelineAsset()
{
    if (m_TimelineRes.m_TimelineAssetPath.empty())
    {
        return false;
    }

    // 加载Timeline资源
    m_TimelineAsset = GET_SYSTEM(AssetManager)->loadAsset<TimelineAsset>(m_TimelineRes.m_TimelineAssetPath);
    if (m_TimelineAsset != nullptr)
    {
        m_AssetLoaded = true;
        return true;
    }

    return false;
}