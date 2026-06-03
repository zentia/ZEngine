#pragma once

#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Core/Math/Vector3.h"

#include <EASTL/unordered_map.h>
#include "Runtime/Function/Framework/World/WorldPartition.h"
#include "Runtime/Resource/ResType/Common/World.h"

#include <filesystem>

class Level;
class LevelDebugger;
class PhysicsScene;
struct Vector3;

/// Manage game worlds and levels. Supports optional UE-style World Partition cell streaming.
class WorldManager : public IEngineSystem
{
    friend class WorldPartition;

public:
    std::string GetName() const override { return "WorldManager"; }
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::Gameplay; }

    bool Initialize() override;
    void Shutdown() override;

    void ReloadCurrentLevel();
    void SaveCurrentLevel();

    void Tick(float delta_time);
    Level* getCurrentActiveLevel() const { return m_CurrentActiveLevel; }

    bool IsWorldPartitionEnabled() const { return m_WorldPartition.IsEnabled(); }
    size_t GetLoadedWorldPartitionCellCount() const { return m_WorldPartition.GetLoadedCellCount(); }
    size_t GetPendingWorldPartitionCellCount() const { return m_WorldPartition.GetPendingCellCount(); }
    const WorldPartition& GetWorldPartition() const { return m_WorldPartition; }
    Vector3 GetWorldPartitionStreamingSource() const;
    eastl::vector<eastl::string> GetLoadedLevelUrls() const;

    std::weak_ptr<PhysicsScene> GetCurrentActivePhysicsScene() const;

    // Used by WorldPartition to stream cells (one Level instance per loaded cell).
    bool LoadLevelAdditive(const eastl::string& level_url);
    void UnloadLevelByUrl(const eastl::string& level_url);
    bool AcquireLevelUrl(const eastl::string& level_url);
    void ReleaseLevelUrl(const eastl::string& level_url);
    void StartPreloadLevelUrl(const eastl::string& level_url);
    bool IsLevelUrlReadyForActivation(const eastl::string& level_url) const;
    void RefreshActiveLevelAfterPartitionUpdate();

private:
    bool LoadWorld(const eastl::string& world_url);
    bool LoadLevel(const eastl::string& level_url);

    void TickLoadedLevels(float delta_time);
    void UpdateWorldPartitionStreaming();

    bool m_IsWorldLoaded {false};
    eastl::string m_CurrentWorldUrl;
    WorldRes* m_CurrentWorldResource {nullptr};

    eastl::unordered_map<eastl::string, Level*> m_LoadedLevels;
    Level* m_CurrentActiveLevel {nullptr};

    WorldPartition m_WorldPartition;
    eastl::unordered_map<eastl::string, int> m_LevelUrlRefCounts;
    std::shared_ptr<LevelDebugger> m_LevelDebugger;
};
