#pragma once

#include "Runtime/Resource/ResType/Common/WorldPartitionTypes.h"

#include <EASTL/unordered_map.h>
#include <unordered_set>

class WorldManager;
class WorldRes;
struct Vector3;

// Runtime World Partition streaming (UE UWorldPartition-style cell load/unload).
class WorldPartition
{
public:
    void InitializeFromWorldRes(const WorldRes& world_res);
    void Shutdown();

    bool IsEnabled() const { return m_Settings.m_Enabled; }
    const WorldPartitionSettings& GetSettings() const { return m_Settings; }

    void UpdateStreaming(WorldManager& world_manager, const Vector3& streaming_source);

    size_t GetLoadedCellCount() const { return m_LoadedCells.size(); }
    size_t GetPendingCellCount() const { return m_PendingCells.size(); }

    WorldPartitionCellCoord WorldToCell(const Vector3& world_position) const;

    void CollectCellsForDebug(const Vector3& streaming_source,
                              std::vector<WorldPartitionCellCoord>& out_loaded,
                              std::vector<WorldPartitionCellCoord>& out_pending,
                              std::vector<WorldPartitionCellCoord>& out_desired) const;

private:
    eastl::string ResolveCellLevelUrl(const WorldPartitionCellCoord& coord) const;
    bool IsCellAuthoringAllowed(const WorldPartitionCellCoord& coord) const;

    std::unordered_set<WorldPartitionCellCoord, WorldPartitionCellCoordHash> CollectDesiredCells(
        const WorldPartitionCellCoord& source_cell) const;

    void UnloadCellsOutsideDesired(WorldManager& world_manager,
                                   const std::unordered_set<WorldPartitionCellCoord, WorldPartitionCellCoordHash>& desired_cells);

    void RequestCells(WorldManager& world_manager,
                      const std::unordered_set<WorldPartitionCellCoord, WorldPartitionCellCoordHash>& desired_cells);

    void PromoteReadyCells(WorldManager& world_manager);

    WorldPartitionSettings m_Settings;
    eastl::unordered_map<WorldPartitionCellCoord, eastl::string, WorldPartitionCellCoordHash> m_ExplicitCellUrls;
    std::unordered_set<WorldPartitionCellCoord, WorldPartitionCellCoordHash> m_LoadedCells;
    std::unordered_set<WorldPartitionCellCoord, WorldPartitionCellCoordHash> m_PendingCells;
    eastl::unordered_map<WorldPartitionCellCoord, eastl::string, WorldPartitionCellCoordHash> m_PendingCellUrls;
};
