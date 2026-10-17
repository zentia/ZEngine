// =============================================================================
// GameMode — 关卡规则层（UE AGameMode 对应物）。
//
// 由 GameInstance 拥有，随关卡创建/销毁。骨架阶段的职责只有一件：
// 决定本关由哪个实体作为默认角色。
// =============================================================================

#include "Runtime/Function/Framework/GameFramework/GameMode.h"

#include "Runtime/Function/Character/Character.h"
#include "Runtime/Function/Framework/Level/Level.h"
#include "Runtime/Function/Framework/World/WorldManager.h"

GameMode::GameMode(std::string level_url)
    : m_LevelUrl(std::move(level_url))
{
}

GameMode::~GameMode()
{
    Shutdown();
}

bool GameMode::Initialize()
{
    if (m_IsInitialized)
    {
        return true;
    }

    // 没有指定默认角色名是合法的：沿用关卡自己配置的 active character，
    // 由 GameEngine::AutoPossessActiveCharacter 直接取。此时不设
    // PendingPossessTarget，消费方会走关卡默认路径。
    if (m_DefaultCharacterName.empty())
    {
        m_IsInitialized = true;
        LOG_INFO(ZEngine, "GameMode[{}]: using the level's own active character", m_LevelUrl);
        return true;
    }

    if (!ResolveDefaultCharacter())
    {
        LOG_WARNING(ZEngine,
                    "GameMode[{}]: default character '{}' not found; falling back to the level's",
                    m_LevelUrl,
                    m_DefaultCharacterName);
        // 解析失败不算致命：回退到关卡默认角色比让关卡起不来好。
    }

    m_IsInitialized = true;
    return true;
}

void GameMode::Shutdown()
{
    if (!m_IsInitialized)
    {
        return;
    }

    // 关卡实体即将失效，必须清掉引用，否则下一关拿到的是悬空 GObjectID。
    m_PendingPossessTarget = k_invalid_gobject_id;
    m_IsInitialized = false;
}

void GameMode::Tick(float /*delta_time*/)
{
    // 骨架阶段无每帧规则（重生/胜负判定在网络与 GameState 落地后补）。
}

bool GameMode::ResolveDefaultCharacter()
{
    auto* world_manager = GET_SYSTEM(WorldManager);
    if (world_manager == nullptr)
    {
        return false;
    }

    const Level* level = world_manager->getCurrentActiveLevel();
    if (level == nullptr)
    {
        return false;
    }

    // 名字匹配关卡里已解析好的 active character。按 GObjectID 遍历实体表
    // 需要 Level 暴露 m_Gobjects，本骨架先只支持指定关卡主角色；扩展
    // 出生点/队伍选择时在这里加分支即可。
    std::shared_ptr<Character> character = level->getCurrentActiveCharacter().lock();
    if (character == nullptr)
    {
        return false;
    }

    std::shared_ptr<GameObject> character_object = character->getObject().lock();
    if (character_object == nullptr)
    {
        return false;
    }

    // GetName() 返回 eastl::string，与 std::string 比较需要拿到 c_str。
    if (m_DefaultCharacterName != character_object->GetName().c_str())
    {
        return false;
    }

    m_PendingPossessTarget = character->GetObjectID();
    LOG_INFO(ZEngine, "GameMode[{}]: default character '{}' -> object {}", m_LevelUrl,
             m_DefaultCharacterName, m_PendingPossessTarget);
    return true;
}
