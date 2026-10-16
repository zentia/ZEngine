#include "GameInstance.h"

#include "Runtime/Core/Base/Macro.h"

bool GameInstance::Initialize()
{
    m_IsInitialized = true;
    return true;
}

void GameInstance::Shutdown()
{
    m_IsInitialized = false;
}

void GameInstance::Tick(float delta_time)
{
    // 骨架：基类无逐帧逻辑。派生类 override 后若需要基类行为应显式调用。
    (void)delta_time;
}

void GameInstance::OnLevelLoaded(const std::string& level_url)
{
    m_CurrentLevelUrl = level_url;
    LOG_INFO(ZEngine, "GameInstance: level loaded {}", level_url);
}

void GameInstance::OnLevelUnloaded(const std::string& level_url)
{
    if (m_CurrentLevelUrl == level_url)
    {
        m_CurrentLevelUrl.clear();
    }
    LOG_INFO(ZEngine, "GameInstance: level unloaded {}", level_url);
}
