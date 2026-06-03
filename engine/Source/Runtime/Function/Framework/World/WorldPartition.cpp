#include "WorldPartition.h"

#include "WorldManager.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Resource/ResType/Common/World.h"

#include <cmath>

namespace
{
    eastl::string ReplaceToken(eastl::string text, const char* token, const eastl::string& value)
    {
        const size_t token_len = strlen(token);
        size_t pos = 0;
        while ((pos = text.find(token, pos)) != eastl::string::npos)
        {
            text.replace(pos, token_len, value);
            pos += value.size();
        }
        return text;
    }

    eastl::string FormatCellLevelPattern(const eastl::string& pattern, int32_t coord_x, int32_t coord_y)
    {
        eastl::string url = pattern;
        url = ReplaceToken(url, "{coord_x}", eastl::to_string(coord_x).c_str());
        url = ReplaceToken(url, "{coord_y}", eastl::to_string(coord_y).c_str());
        return url;
    }
}  // namespace

void WorldPartition::InitializeFromWorldRes(const WorldRes& world_res)
{
    m_Settings.m_Enabled = world_res.m_EnableWorldPartition;
    m_Settings.m_AsyncCellLoading = world_res.m_AsyncCellLoading;
    m_Settings.m_CellSize = world_res.m_CellSize;
    m_Settings.m_LoadingRange = world_res.m_LoadingRange;
    m_Settings.m_GridOrigin = world_res.m_GridOrigin;
    m_Settings.m_CellLevelUrlPattern = world_res.m_CellLevelUrlPattern;
    m_Settings.m_Cells = world_res.m_PartitionCells;

    m_ExplicitCellUrls.clear();
    for (const WorldPartitionCellEntry& entry : m_Settings.m_Cells)
    {
        WorldPartitionCellCoord coord;
        coord.m_X = entry.m_CoordX;
        coord.m_Y = entry.m_CoordY;
        m_ExplicitCellUrls.emplace(coord, entry.m_LevelUrl);
    }

    m_LoadedCells.clear();
    m_PendingCells.clear();
    m_PendingCellUrls.clear();

    if (m_Settings.m_Enabled)
    {
        LOG_INFO(ZWorld,
                 "World Partition enabled: cell_size={} loading_range={} async={} explicit_cells={} pattern='{}'",
                 m_Settings.m_CellSize,
                 m_Settings.m_LoadingRange,
                 m_Settings.m_AsyncCellLoading,
                 m_ExplicitCellUrls.size(),
                 m_Settings.m_CellLevelUrlPattern.c_str());
    }
}

void WorldPartition::Shutdown()
{
    m_Settings = {};
    m_ExplicitCellUrls.clear();
    m_LoadedCells.clear();
    m_PendingCells.clear();
    m_PendingCellUrls.clear();
}

WorldPartitionCellCoord WorldPartition::WorldToCell(const Vector3& world_position) const
{
    WorldPartitionCellCoord coord;
    if (m_Settings.m_CellSize <= 0.f)
    {
        return coord;
    }

    const float local_x = world_position.x - m_Settings.m_GridOrigin.x;
    const float local_y = world_position.y - m_Settings.m_GridOrigin.y;
    coord.m_X = static_cast<int32_t>(std::floor(local_x / m_Settings.m_CellSize));
    coord.m_Y = static_cast<int32_t>(std::floor(local_y / m_Settings.m_CellSize));
    return coord;
}

eastl::string WorldPartition::ResolveCellLevelUrl(const WorldPartitionCellCoord& coord) const
{
    if (auto it = m_ExplicitCellUrls.find(coord); it != m_ExplicitCellUrls.end())
    {
        return it->second;
    }

    if (!m_Settings.m_CellLevelUrlPattern.empty())
    {
        return FormatCellLevelPattern(m_Settings.m_CellLevelUrlPattern, coord.m_X, coord.m_Y);
    }

    return {};
}

bool WorldPartition::IsCellAuthoringAllowed(const WorldPartitionCellCoord& coord) const
{
    if (!m_ExplicitCellUrls.empty())
    {
        return m_ExplicitCellUrls.find(coord) != m_ExplicitCellUrls.end();
    }
    return !m_Settings.m_CellLevelUrlPattern.empty();
}

std::unordered_set<WorldPartitionCellCoord, WorldPartitionCellCoordHash> WorldPartition::CollectDesiredCells(
    const WorldPartitionCellCoord& source_cell) const
{
    std::unordered_set<WorldPartitionCellCoord, WorldPartitionCellCoordHash> desired;
    const int32_t range = std::max<int32_t>(0, m_Settings.m_LoadingRange);
    for (int32_t dy = -range; dy <= range; ++dy)
    {
        for (int32_t dx = -range; dx <= range; ++dx)
        {
            WorldPartitionCellCoord coord;
            coord.m_X = source_cell.m_X + dx;
            coord.m_Y = source_cell.m_Y + dy;
            if (IsCellAuthoringAllowed(coord))
            {
                desired.insert(coord);
            }
        }
    }
    return desired;
}

void WorldPartition::CollectCellsForDebug(const Vector3& streaming_source,
                                            std::vector<WorldPartitionCellCoord>& out_loaded,
                                            std::vector<WorldPartitionCellCoord>& out_pending,
                                            std::vector<WorldPartitionCellCoord>& out_desired) const
{
    out_loaded.clear();
    out_pending.clear();
    out_desired.clear();

    const auto desired = CollectDesiredCells(WorldToCell(streaming_source));
    out_loaded.reserve(m_LoadedCells.size());
    for (const WorldPartitionCellCoord& coord : m_LoadedCells)
    {
        out_loaded.push_back(coord);
    }
    out_pending.reserve(m_PendingCells.size());
    for (const WorldPartitionCellCoord& coord : m_PendingCells)
    {
        out_pending.push_back(coord);
    }
    out_desired.reserve(desired.size());
    for (const WorldPartitionCellCoord& coord : desired)
    {
        out_desired.push_back(coord);
    }
}

void WorldPartition::UnloadCellsOutsideDesired(
    WorldManager& world_manager,
    const std::unordered_set<WorldPartitionCellCoord, WorldPartitionCellCoordHash>& desired_cells)
{
    eastl::vector<WorldPartitionCellCoord> cells_to_unload;
    cells_to_unload.reserve(m_LoadedCells.size());
    for (const WorldPartitionCellCoord& loaded : m_LoadedCells)
    {
        if (desired_cells.find(loaded) == desired_cells.end())
        {
            cells_to_unload.push_back(loaded);
        }
    }

    for (const WorldPartitionCellCoord& coord : cells_to_unload)
    {
        const eastl::string level_url = ResolveCellLevelUrl(coord);
        if (!level_url.empty())
        {
            world_manager.ReleaseLevelUrl(level_url);
            LOG_INFO(ZWorld, "World Partition unloaded cell ({}, {}) -> {}", coord.m_X, coord.m_Y, level_url.c_str());
        }
        m_LoadedCells.erase(coord);
    }

    eastl::vector<WorldPartitionCellCoord> pending_to_cancel;
    for (const WorldPartitionCellCoord& pending : m_PendingCells)
    {
        if (desired_cells.find(pending) == desired_cells.end())
        {
            pending_to_cancel.push_back(pending);
        }
    }
    for (const WorldPartitionCellCoord& coord : pending_to_cancel)
    {
        m_PendingCells.erase(coord);
        m_PendingCellUrls.erase(coord);
    }
}

void WorldPartition::RequestCells(
    WorldManager& world_manager,
    const std::unordered_set<WorldPartitionCellCoord, WorldPartitionCellCoordHash>& desired_cells)
{
    for (const WorldPartitionCellCoord& coord : desired_cells)
    {
        if (m_LoadedCells.find(coord) != m_LoadedCells.end() || m_PendingCells.find(coord) != m_PendingCells.end())
        {
            continue;
        }

        const eastl::string level_url = ResolveCellLevelUrl(coord);
        if (level_url.empty())
        {
            continue;
        }

        if (m_Settings.m_AsyncCellLoading)
        {
            world_manager.StartPreloadLevelUrl(level_url);
            m_PendingCells.insert(coord);
            m_PendingCellUrls.emplace(coord, level_url);
            LOG_INFO(ZWorld,
                     "World Partition preloading cell ({}, {}) -> {}",
                     coord.m_X,
                     coord.m_Y,
                     level_url.c_str());
        }
        else if (world_manager.AcquireLevelUrl(level_url))
        {
            m_LoadedCells.insert(coord);
            LOG_INFO(ZWorld,
                     "World Partition loaded cell ({}, {}) -> {}",
                     coord.m_X,
                     coord.m_Y,
                     level_url.c_str());
        }
        else
        {
            LOG_WARNING(ZWorld,
                        "World Partition failed to load cell ({}, {}) -> {}",
                        coord.m_X,
                        coord.m_Y,
                        level_url.c_str());
        }
    }
}

void WorldPartition::PromoteReadyCells(WorldManager& world_manager)
{
    eastl::vector<WorldPartitionCellCoord> ready_cells;
    ready_cells.reserve(m_PendingCells.size());

    for (const WorldPartitionCellCoord& coord : m_PendingCells)
    {
        auto url_it = m_PendingCellUrls.find(coord);
        if (url_it == m_PendingCellUrls.end())
        {
            ready_cells.push_back(coord);
            continue;
        }

        if (world_manager.IsLevelUrlReadyForActivation(url_it->second))
        {
            ready_cells.push_back(coord);
        }
    }

    for (const WorldPartitionCellCoord& coord : ready_cells)
    {
        auto url_it = m_PendingCellUrls.find(coord);
        if (url_it == m_PendingCellUrls.end())
        {
            m_PendingCells.erase(coord);
            continue;
        }

        const eastl::string& level_url = url_it->second;
        if (world_manager.AcquireLevelUrl(level_url))
        {
            m_LoadedCells.insert(coord);
            LOG_INFO(ZWorld,
                     "World Partition activated cell ({}, {}) -> {}",
                     coord.m_X,
                     coord.m_Y,
                     level_url.c_str());
        }
        else
        {
            LOG_WARNING(ZWorld,
                        "World Partition failed to activate cell ({}, {}) -> {}",
                        coord.m_X,
                        coord.m_Y,
                        level_url.c_str());
        }

        m_PendingCells.erase(coord);
        m_PendingCellUrls.erase(coord);
    }
}

void WorldPartition::UpdateStreaming(WorldManager& world_manager, const Vector3& streaming_source)
{
    if (!m_Settings.m_Enabled)
    {
        return;
    }

    if (m_Settings.m_AsyncCellLoading)
    {
        PromoteReadyCells(world_manager);
    }

    const WorldPartitionCellCoord source_cell = WorldToCell(streaming_source);
    const auto desired_cells = CollectDesiredCells(source_cell);

    UnloadCellsOutsideDesired(world_manager, desired_cells);
    RequestCells(world_manager, desired_cells);

    world_manager.RefreshActiveLevelAfterPartitionUpdate();
}
