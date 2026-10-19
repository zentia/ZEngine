#include "GameEngine.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/Function/Character/Character.h"
#include "Runtime/Function/Framework/GameFramework/GameMode.h"
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
    BindToActiveLevel();

    m_IsInitialized = true;
    LOG_INFO(ZEngine, "GameEngine initialized ({} local player(s))", m_LocalPlayers.size());
    return true;
}

void GameEngine::BindToActiveLevel()
{
    if (m_GameInstance == nullptr)
    {
        return;
    }

    auto* world_manager = GET_SYSTEM(WorldManager);
    if (world_manager == nullptr || !world_manager->IsWorldLoaded())
    {
        return;
    }

    const Level* level = world_manager->getCurrentActiveLevel();
    if (level == nullptr)
    {
        return;
    }

    m_GameInstance->OnLevelLoaded(level->getLevelResUrl().c_str());
    AutoPossessActiveCharacter();
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

    if (m_GameInstance != nullptr)
    {
        m_GameInstance->Shutdown();
        m_GameInstance.reset();
    }

    m_IsInPIE = false;
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

        // GameMode 只在本关存活，随 GameInstance 的关卡生命周期创建/销毁。
        if (GameMode* game_mode = m_GameInstance->GetGameMode(); game_mode != nullptr)
        {
            game_mode->Tick(delta_time);
        }
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

    // GameMode 可以指定本关的默认角色；没指定时沿用关卡自己的 active character。
    if (m_GameInstance != nullptr)
    {
        if (const GameMode* game_mode = m_GameInstance->GetGameMode(); game_mode != nullptr)
        {
            const GObjectID target = game_mode->GetPendingPossessTarget();
            if (target != k_invalid_gobject_id)
            {
                LocalPlayer* first_player = m_LocalPlayers.empty() ? nullptr : m_LocalPlayers.front().get();
                PlayerController* controller =
                    first_player != nullptr ? first_player->GetPlayerController() : nullptr;
                if (controller != nullptr)
                {
                    return controller->Possess(target);
                }
                return false;
            }
        }
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

bool GameEngine::BeginPIE()
{
    if (m_IsInPIE)
    {
        return true;
    }

    // 游戏层按需建立：编辑器启动时没有 GameEngine（RegisterGameRuntime 只在
    // 独立进程调用），所以 PIE 第一次进入时这里还是未初始化状态。
    if (!m_IsInitialized && !Initialize())
    {
        LOG_ERROR(ZEngine, "GameEngine::BeginPIE: game layer failed to initialize");
        return false;
    }

    auto* world_manager = GET_SYSTEM(WorldManager);
    if (world_manager == nullptr)
    {
        LOG_ERROR(ZEngine, "GameEngine::BeginPIE: WorldManager is not registered");
        return false;
    }

    // UE 式独立 Play World：先把编辑器关卡复制出第二份并切成活动关卡，
    // 之后所有游戏逻辑都只作用于那一份，编辑器世界保持不动。
    // 只有复制失败（无关卡 / 落盘失败 / WP 模式）才退化为同世界播放 ——
    // 退化时必须在停止时重载才能丢弃运行期改动。
    eastl::string play_world_url;
    const bool has_play_world = world_manager->BeginPlayWorld(play_world_url);
    m_PieUsedPlayWorld = has_play_world;
    if (!has_play_world)
    {
        const Level* fallback_level = world_manager->getCurrentActiveLevel();
        if (fallback_level == nullptr)
        {
            LOG_ERROR(ZEngine, "GameEngine::BeginPIE: no active level to play");
            return false;
        }
        LOG_WARNING(ZEngine, "GameEngine::BeginPIE: falling back to same-world PIE on {}",
                    fallback_level->getLevelResUrl().c_str());
    }

    // 关卡切换后 GameInstance 记住的是新的 URL（Play World 那份），并据此建
    // 本关 GameMode、接管角色。接管必须发生在 Play World 成为活动关卡之后。
    BindToActiveLevel();

    m_IsInPIE = true;
    LOG_INFO(ZEngine, "GameEngine: PIE started on {}", world_manager->getCurrentActiveLevel() != nullptr
        ? world_manager->getCurrentActiveLevel()->getLevelResUrl().c_str() : "(none)");
    return true;
}

void GameEngine::EndPIE()
{
    if (!m_IsInPIE)
    {
        return;
    }

    // 关卡即将被卸载，实体会失效：先解绑，避免 GObjectID 悬空。
    ReleaseAllPossessions();
    if (m_GameInstance != nullptr)
    {
        m_GameInstance->OnLevelUnloaded(m_GameInstance->GetCurrentLevelUrl());
    }

    // 独立 Play World 直接丢弃即可；游戏逻辑从未碰过编辑器那份关卡。
    // 若当初复制失败退化成了同世界播放，这里 EndPlayWorld() 是空操作，
    // 由调用方（Editor::ExitPIE）负责重载恢复。
    if (auto* world_manager = GET_SYSTEM(WorldManager); world_manager != nullptr)
    {
        world_manager->EndPlayWorld();
    }

    m_IsInPIE = false;
    m_PieUsedPlayWorld = false;
    LOG_INFO(ZEngine, "GameEngine: PIE stopped");
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
