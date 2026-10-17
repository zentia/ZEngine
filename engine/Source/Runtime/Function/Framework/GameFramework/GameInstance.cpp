#include "GameInstance.h"

#include "Runtime/Core/Base/Macro.h"

#include <memory>
#include <utility>

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

    // GameMode 每关一份：在这里创建，OnLevelUnloaded 里销毁。
    // 先销毁旧的，避免 LoadLevel 连续调用时泄漏。
    m_GameMode.reset();
    m_GameMode = CreateGameMode(level_url);
    if (m_GameMode != nullptr)
    {
        m_GameMode->Initialize();
    }

    LOG_INFO(ZEngine, "GameInstance: level loaded {}", level_url);
}

void GameInstance::OnLevelUnloaded(const std::string& level_url)
{
    if (m_CurrentLevelUrl == level_url)
    {
        m_CurrentLevelUrl.clear();
    }

    // 关卡实体即将失效：GameMode::Shutdown() 清掉持有的 GObjectID。
    if (m_GameMode != nullptr)
    {
        m_GameMode->Shutdown();
        m_GameMode.reset();
    }

    LOG_INFO(ZEngine, "GameInstance: level unloaded {}", level_url);
}

std::unique_ptr<GameMode> GameInstance::CreateGameMode(const std::string& level_url)
{
    return std::make_unique<GameMode>(level_url);
}
