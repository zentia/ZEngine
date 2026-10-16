#include "GameEngine.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/Function/Character/Character.h"
#include "Runtime/Function/Framework/GameFramework/PlayerController.h"
#include "Runtime/Function/Framework/Level/Level.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "Runtime/Function/Input/InputSystem.h"
#include "Runtime/Function/Render/WindowSystem.h"

#include <chrono>
#include <thread>

GameEngine::GameEngine() = default;

GameEngine::~GameEngine()
{
    Shutdown();
}

std::vector<std::type_index> GameEngine::GetDependencies() const
{
    // 关卡必须先于本系统可用：Initialize() 里要读活动关卡的角色。
    // InputSystem 必须在 PlayerController::UpdateInput() 之前起来。
    return {GET_SYSTEM_TYPE(WorldManager), GET_SYSTEM_TYPE(InputSystem)};
}

bool GameEngine::Initialize()
{
    if (m_IsInitialized)
    {
        return true;
    }

    m_GameInstance = std::make_unique<GameInstance>();
    if (!m_GameInstance->Initialize())
    {
        LOG_ERROR(ZEngine, "GameEngine: GameInstance initialization failed");
        m_GameInstance.reset();
        return false;
    }

    // 独立进程默认单人。分屏由命令行或后续配置驱动 CreateLocalPlayers(n>1)。
    if (!CreateLocalPlayers(1))
    {
        LOG_ERROR(ZEngine, "GameEngine: failed to create the default local player");
        m_GameInstance->Shutdown();
        m_GameInstance.reset();
        return false;
    }

    // 若关卡已在系统就绪前被加载（独立进程指定了启动关卡），此时补上接管。
    if (auto* world_manager = GET_SYSTEM(WorldManager); world_manager != nullptr && world_manager->IsWorldLoaded())
    {
        if (const Level* level = world_manager->getCurrentActiveLevel(); level != nullptr)
        {
            m_GameInstance->OnLevelLoaded(level->getLevelResUrl().c_str());
            AutoPossessActiveCharacter();
        }
    }

    m_IsInitialized = true;
    LOG_INFO(ZEngine, "GameEngine initialized ({} local player(s))", m_LocalPlayers.size());
    return true;
}

void GameEngine::Shutdown()
{
    if (!m_IsInitialized)
    {
        return;
    }

    ReleaseAllPossessions();

    for (auto& player : m_LocalPlayers)
    {
        player->Shutdown();
    }
    m_LocalPlayers.clear();

    if (m_GameInstance != nullptr)
    {
        m_GameInstance->Shutdown();
        m_GameInstance.reset();
    }

    m_IsInitialized = false;
    LOG_INFO(ZEngine, "GameEngine shut down");
}

void GameEngine::Tick(float delta_time)
{
    if (!m_IsInitialized)
    {
        return;
    }

    if (m_GameInstance != nullptr)
    {
        m_GameInstance->Tick(delta_time);
    }

    for (auto& player : m_LocalPlayers)
    {
        player->Tick(delta_time);
    }
}

int GameEngine::Run()
{
    auto* window_system = GET_SYSTEM(WindowSystem);
    if (window_system == nullptr)
    {
        LOG_ERROR(ZEngine, "GameEngine::Run: WindowSystem is not registered");
        return 1;
    }

    // 与 Application::Run() 结构一致，区别只在每帧末尾多跑一次游戏层 Tick：
    // GameInstance（跨关卡数据）→ 各 LocalPlayer（→ 其 PlayerController）。
    // 放在 TickOneFrame 之后，保证本帧的输入与物理已经推进过。
    while (!window_system->ShouldClose() && !m_RequestQuit)
    {
        const float delta_time = CalculateDeltaTime();
        if (!TickOneFrame(delta_time))
        {
            break;
        }

        Tick(delta_time);
    }

    return 0;
}

bool GameEngine::CreateLocalPlayers(int num_players)
{
    if (num_players <= 0)
    {
        LOG_ERROR(ZEngine, "GameEngine::CreateLocalPlayers: invalid count {}", num_players);
        return false;
    }

    const int existing = static_cast<int>(m_LocalPlayers.size());
    for (int i = existing; i < existing + num_players; ++i)
    {
        auto player = std::make_unique<LocalPlayer>(i);
        if (!player->Initialize())
        {
            LOG_ERROR(ZEngine, "GameEngine: LocalPlayer[{}] initialization failed", i);
            return false;
        }
        m_LocalPlayers.push_back(std::move(player));
    }

    return true;
}

LocalPlayer* GameEngine::GetLocalPlayer(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_LocalPlayers.size()))
    {
        return nullptr;
    }
    return m_LocalPlayers[static_cast<size_t>(index)].get();
}

bool GameEngine::LoadLevel(const std::string& level_url)
{
    auto* world_manager = GET_SYSTEM(WorldManager);
    if (world_manager == nullptr)
    {
        LOG_ERROR(ZEngine, "GameEngine::LoadLevel: WorldManager is not registered");
        return false;
    }

    // 旧关卡的实体即将失效：先解绑，避免 GObjectID 指向已卸载的关卡。
    ReleaseAllPossessions();
    if (m_GameInstance != nullptr && !m_GameInstance->GetCurrentLevelUrl().empty())
    {
        m_GameInstance->OnLevelUnloaded(m_GameInstance->GetCurrentLevelUrl());
    }

    if (!world_manager->OpenScene(level_url.c_str()))
    {
        LOG_ERROR(ZEngine, "GameEngine::LoadLevel: OpenScene failed for {}", level_url);
        return false;
    }

    if (m_GameInstance != nullptr)
    {
        m_GameInstance->OnLevelLoaded(level_url);
    }

    AutoPossessActiveCharacter();
    return true;
}

bool GameEngine::AutoPossessActiveCharacter()
{
    auto* world_manager = GET_SYSTEM(WorldManager);
    if (world_manager == nullptr)
    {
        return false;
    }

    const Level* level = world_manager->getCurrentActiveLevel();
    if (level == nullptr)
    {
        LOG_WARNING(ZEngine, "GameEngine::AutoPossessActiveCharacter: no active level");
        return false;
    }

    std::shared_ptr<Character> character = level->getCurrentActiveCharacter().lock();
    if (character == nullptr)
    {
        LOG_WARNING(ZEngine, "GameEngine::AutoPossessActiveCharacter: level has no active character");
        return false;
    }

    if (m_LocalPlayers.empty())
    {
        return false;
    }

    LocalPlayer* first_player = m_LocalPlayers.front().get();
    PlayerController* controller = first_player->GetPlayerController();
    if (controller == nullptr)
    {
        return false;
    }

    return controller->Possess(character->GetObjectID());
}

void GameEngine::ReleaseAllPossessions()
{
    for (auto& player : m_LocalPlayers)
    {
        if (PlayerController* controller = player->GetPlayerController(); controller != nullptr)
        {
            controller->UnPossess();
        }
    }
}
