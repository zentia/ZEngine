#include "TimelineComponent.h"

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Function/Framework/Level/Level.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/ResType/Data/TimelineAsset.h"
#include "Runtime/Resource/ResType/Data/TimelineAssetIO.h"

void TimelineComponent::PostLoadResource(GameObject* parent_object)
{
    Component::PostLoadResource(parent_object);

    // A reload replaces the director: stop the old one first so it restores what its
    // active clips applied (activation flags, animation overrides, voices) instead of
    // leaving them behind.
    if (m_Director)
    {
        m_Director->Stop();
    }

    // 创建Director
    m_Director = std::make_shared<TimelineDirector>();
    m_Director->SetEventHandler(m_EventHandler);

    // 加载Timeline资源
    if (LoadTimelineAsset())
    {
        // 获取Level（通过WorldManager）。没有 WorldManager（编辑器工具、无头测试实例化 prefab）
        // 与"没有活动关卡"同样处理：level 留空，Director 自己决定能不能初始化。
        Level* level = nullptr;
        if (WorldManager* world_manager = GET_SYSTEM(WorldManager))
        {
            level = world_manager->getCurrentActiveLevel();
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

void TimelineComponent::SetEventHandler(TimelineEventHandler handler)
{
    m_EventHandler = std::move(handler);
    if (m_Director)
    {
        m_Director->SetEventHandler(m_EventHandler);
    }
}

bool TimelineComponent::LoadTimelineAsset()
{
    if (m_TimelineRes.m_TimelineAssetPath.empty())
    {
        return false;
    }

    // Timeline 是多对象文件（资产 + 轨道 + 片段），必须整图读取；
    // loadAsset<TimelineAsset> 只会读出 fileID 1，轨道/片段的 PPtr 全部悬空。
    // 路径解析与 Level 一致：优先工程 Assets/，找不到再回落到引擎资源目录。
    AssetManager* asset_manager = GET_SYSTEM(AssetManager);
    if (asset_manager == nullptr)
    {
        return false;
    }
    std::filesystem::path timeline_path = asset_manager->ResolveProjectContentPath(m_TimelineRes.m_TimelineAssetPath);
    std::error_code ec;
    if (!std::filesystem::exists(timeline_path, ec))
    {
        timeline_path = asset_manager->GetFullPath(m_TimelineRes.m_TimelineAssetPath);
    }
    m_TimelineAsset = TimelineAssetIO::Load(*asset_manager, timeline_path);
    if (m_TimelineAsset != nullptr)
    {
        m_AssetLoaded = true;
        return true;
    }

    return false;
}