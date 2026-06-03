#include "WorldManager.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Character/Character.h"
#include "Runtime/Function/Framework/Component/Camera/CameraComponent.h"
#include "Runtime/Function/Framework/Component/Transform/TransformComponent.h"
#include "Runtime/Function/Framework/Level/Level.h"
#include "Runtime/Function/Framework/Level/LevelDebugger.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/Config/ConfigManager.h"
#include "Runtime/Resource/Preload/PreloadManager.h"

bool WorldManager::Initialize()
{
    m_IsWorldLoaded = false;
    m_CurrentWorldUrl = GET_SYSTEM(ConfigManager)->GetDefaultWorldUrl();
    m_LevelDebugger = std::make_shared<LevelDebugger>();
    return true;
}

void WorldManager::Shutdown()
{
    m_WorldPartition.Shutdown();

    for (auto& level_pair : m_LoadedLevels)
    {
        if (level_pair.second != nullptr)
        {
            level_pair.second->Unload();
        }
    }
    m_LoadedLevels.clear();
    m_LevelUrlRefCounts.clear();

    if (m_CurrentActiveLevel != nullptr)
    {
        MemoryManager::DestroyObject(m_CurrentActiveLevel);
        m_CurrentActiveLevel = nullptr;
    }

    m_CurrentWorldResource = nullptr;
    m_CurrentWorldUrl.clear();
    m_IsWorldLoaded = false;
    m_LevelDebugger.reset();
}

void WorldManager::Tick(float delta_time)
{
    if (!m_IsWorldLoaded)
    {
        LoadWorld(m_CurrentWorldUrl);
    }

    if (m_WorldPartition.IsEnabled())
    {
        UpdateWorldPartitionStreaming();
        TickLoadedLevels(delta_time);
    }
    else if (m_CurrentActiveLevel != nullptr)
    {
        m_CurrentActiveLevel->Tick(delta_time);
        m_LevelDebugger->Tick(m_CurrentActiveLevel);
    }
}

eastl::vector<eastl::string> WorldManager::GetLoadedLevelUrls() const
{
    eastl::vector<eastl::string> urls;
    urls.reserve(m_LoadedLevels.size());
    for (const auto& level_pair : m_LoadedLevels)
    {
        urls.push_back(level_pair.first);
    }
    return urls;
}

void WorldManager::TickLoadedLevels(float delta_time)
{
    for (auto& level_pair : m_LoadedLevels)
    {
        Level* level = level_pair.second;
        if (level == nullptr)
        {
            continue;
        }
        level->Tick(delta_time);
    }

    if (m_CurrentActiveLevel != nullptr)
    {
        m_LevelDebugger->Tick(m_CurrentActiveLevel);
    }
}

void WorldManager::UpdateWorldPartitionStreaming()
{
    m_WorldPartition.UpdateStreaming(*this, GetWorldPartitionStreamingSource());
}

Vector3 WorldManager::GetWorldPartitionStreamingSource() const
{
    for (const auto& level_pair : m_LoadedLevels)
    {
        Level* level = level_pair.second;
        if (level == nullptr)
        {
            continue;
        }

        if (std::shared_ptr<Character> character = level->getCurrentActiveCharacter().lock())
        {
            return character->GetPosition();
        }
    }

    for (const auto& level_pair : m_LoadedLevels)
    {
        Level* level = level_pair.second;
        if (level == nullptr)
        {
            continue;
        }

        if (CameraComponent* camera = level->GetMainCameraComponent())
        {
            if (GameObject* camera_object = camera->GetParentObject())
            {
                if (TransformComponent* transform = camera_object->tryGetComponent(TransformComponent))
                {
                    return transform->GetWorldPosition();
                }
            }
        }
    }

    return m_WorldPartition.GetSettings().m_GridOrigin;
}

std::weak_ptr<PhysicsScene> WorldManager::GetCurrentActivePhysicsScene() const
{
    Level* active_level = m_CurrentActiveLevel;
    if (!active_level)
    {
        return {};
    }
    return active_level->getPhysicsScene();
}

bool WorldManager::LoadWorld(const eastl::string& world_url)
{
    LOG_INFO(ZWorld, "loading world: {}", world_url.c_str());
    WorldRes* world_res = GET_SYSTEM(AssetManager)->loadAsset<WorldRes>(world_url);
    if (!world_res)
    {
        LOG_ERROR(ZWorld, "load world asset failed: {}", world_url.c_str());
        m_CurrentActiveLevel = nullptr;
        return false;
    }

    m_WorldPartition.InitializeFromWorldRes(*world_res);

    if (m_WorldPartition.IsEnabled())
    {
        m_IsWorldLoaded = true;
        MemoryManager::DestroyObject(world_res);
        UpdateWorldPartitionStreaming();
        LOG_INFO(ZWorld, "world load succeed (World Partition streaming)");
        return true;
    }

    const bool is_level_load_success = LoadLevel(world_res->m_DefaultLevelUrl);
    if (!is_level_load_success)
    {
        LOG_ERROR(ZWorld, "load level failed: {}", world_res->m_DefaultLevelUrl.c_str());
        m_CurrentActiveLevel = nullptr;
        MemoryManager::DestroyObject(world_res);
        return false;
    }

    auto iter = m_LoadedLevels.find(world_res->m_DefaultLevelUrl);
    ASSERT(iter != m_LoadedLevels.end());
    m_CurrentActiveLevel = iter->second;

    m_IsWorldLoaded = true;
    MemoryManager::DestroyObject(world_res);
    LOG_INFO(ZWorld, "world load succeed!");
    return true;
}

bool WorldManager::LoadLevel(const eastl::string& level_url)
{
    if (auto existing = m_LoadedLevels.find(level_url); existing != m_LoadedLevels.end())
    {
        m_CurrentActiveLevel = existing->second;
        return m_CurrentActiveLevel != nullptr;
    }

    return LoadLevelAdditive(level_url);
}

bool WorldManager::LoadLevelAdditive(const eastl::string& level_url)
{
    if (level_url.empty())
    {
        return false;
    }

    if (auto existing = m_LoadedLevels.find(level_url); existing != m_LoadedLevels.end())
    {
        return existing->second != nullptr;
    }

    Level* new_level = MemoryManager::CreateObject<Level>();
    const bool is_level_load_success = new_level->load(level_url);
    if (!is_level_load_success)
    {
        MemoryManager::DestroyObject(new_level);
        LOG_ERROR(ZWorld, "load level failed: {}", level_url.c_str());
        return false;
    }

    m_LoadedLevels.emplace(level_url, new_level);
    if (m_CurrentActiveLevel == nullptr)
    {
        m_CurrentActiveLevel = new_level;
    }
    return true;
}

void WorldManager::UnloadLevelByUrl(const eastl::string& level_url)
{
    auto iter = m_LoadedLevels.find(level_url);
    if (iter == m_LoadedLevels.end())
    {
        return;
    }

    Level* level = iter->second;
    if (level != nullptr)
    {
        level->Unload();
        MemoryManager::DestroyObject(level);
    }

    if (m_CurrentActiveLevel == level)
    {
        m_CurrentActiveLevel = nullptr;
    }

    m_LoadedLevels.erase(iter);
    m_LevelUrlRefCounts.erase(level_url);
}

bool WorldManager::AcquireLevelUrl(const eastl::string& level_url)
{
    if (level_url.empty())
    {
        return false;
    }

    auto ref_it = m_LevelUrlRefCounts.find(level_url);
    if (ref_it != m_LevelUrlRefCounts.end() && ref_it->second > 0)
    {
        ++ref_it->second;
        return true;
    }

    if (!LoadLevelAdditive(level_url))
    {
        return false;
    }

    m_LevelUrlRefCounts[level_url] = 1;
    return true;
}

void WorldManager::ReleaseLevelUrl(const eastl::string& level_url)
{
    if (level_url.empty())
    {
        return;
    }

    auto ref_it = m_LevelUrlRefCounts.find(level_url);
    if (ref_it == m_LevelUrlRefCounts.end() || ref_it->second <= 0)
    {
        return;
    }

    --ref_it->second;
    if (ref_it->second > 0)
    {
        return;
    }

    m_LevelUrlRefCounts.erase(ref_it);
    UnloadLevelByUrl(level_url);
}

void WorldManager::StartPreloadLevelUrl(const eastl::string& level_url)
{
    if (level_url.empty())
    {
        return;
    }

    if (auto preload = GET_SYSTEM(PreloadManager))
    {
        preload->PreloadAsset(level_url, PreloadPriority::High);
    }
}

bool WorldManager::IsLevelUrlReadyForActivation(const eastl::string& level_url) const
{
    if (level_url.empty())
    {
        return false;
    }

    if (m_LoadedLevels.find(level_url) != m_LoadedLevels.end())
    {
        return true;
    }

    if (auto preload = GET_SYSTEM(PreloadManager))
    {
        return preload->IsAssetLoaded(level_url);
    }

    return false;
}

void WorldManager::RefreshActiveLevelAfterPartitionUpdate()
{
    Level* preferred = nullptr;

    for (auto& level_pair : m_LoadedLevels)
    {
        Level* level = level_pair.second;
        if (level == nullptr)
        {
            continue;
        }

        if (level->getCurrentActiveCharacter().lock())
        {
            preferred = level;
            break;
        }
    }

    if (preferred == nullptr)
    {
        for (auto& level_pair : m_LoadedLevels)
        {
            Level* level = level_pair.second;
            if (level != nullptr && level->GetMainCameraComponent() != nullptr)
            {
                preferred = level;
                break;
            }
        }
    }

    if (preferred == nullptr && !m_LoadedLevels.empty())
    {
        preferred = m_LoadedLevels.begin()->second;
    }

    m_CurrentActiveLevel = preferred;
}

void WorldManager::ReloadCurrentLevel()
{
    if (m_WorldPartition.IsEnabled())
    {
        for (auto& level_pair : m_LoadedLevels)
        {
            if (level_pair.second != nullptr)
            {
                level_pair.second->Unload();
                MemoryManager::DestroyObject(level_pair.second);
            }
        }
        m_LoadedLevels.clear();
        m_LevelUrlRefCounts.clear();
        m_CurrentActiveLevel = nullptr;

        WorldRes* world_res = GET_SYSTEM(AssetManager)->loadAsset<WorldRes>(m_CurrentWorldUrl);
        if (!world_res)
        {
            LOG_ERROR(ZWorld, "reload failed: cannot load world {}", m_CurrentWorldUrl.c_str());
            return;
        }
        m_WorldPartition.Shutdown();
        m_WorldPartition.InitializeFromWorldRes(*world_res);
        MemoryManager::DestroyObject(world_res);
        UpdateWorldPartitionStreaming();
        LOG_INFO(ZWorld, "reload world partition cells requested");
        return;
    }

    auto active_level = m_CurrentActiveLevel;
    if (active_level == nullptr)
    {
        LOG_WARNING(ZWorld, "current level is nil");
        return;
    }

    const eastl::string level_url = active_level->getLevelResUrl();
    UnloadLevelByUrl(level_url);

    const bool is_load_success = LoadLevel(level_url);
    if (!is_load_success)
    {
        LOG_ERROR(ZWorld, "load level failed {}", level_url.c_str());
        return;
    }

    auto iter = m_LoadedLevels.find(level_url);
    ASSERT(iter != m_LoadedLevels.end());
    m_CurrentActiveLevel = iter->second;

    LOG_INFO(ZWorld, "reload current level succeed");
}

void WorldManager::SaveCurrentLevel()
{
    if (m_WorldPartition.IsEnabled())
    {
        for (auto& level_pair : m_LoadedLevels)
        {
            if (level_pair.second != nullptr)
            {
                level_pair.second->save();
            }
        }
        LOG_INFO(ZWorld, "saved {} streamed level(s)", m_LoadedLevels.size());
        return;
    }

    Level* active_level = m_CurrentActiveLevel;
    if (active_level == nullptr)
    {
        LOG_ERROR(ZWorld, "save level failed, no active level");
        return;
    }

    active_level->save();
}
