#pragma once

#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"

#include <cstdint>
#include <vector>

// UE World Partition: one runtime Level instance per spatial cell on the X-Y grid (Z is up).
struct WorldPartitionCellCoord
{
    int32_t m_X {0};
    int32_t m_Y {0};

    bool operator==(const WorldPartitionCellCoord& other) const
    {
        return m_X == other.m_X && m_Y == other.m_Y;
    }
};

struct WorldPartitionCellCoordHash
{
    size_t operator()(const WorldPartitionCellCoord& coord) const noexcept
    {
        const size_t x = static_cast<size_t>(coord.m_X);
        const size_t y = static_cast<size_t>(coord.m_Y);
        return x * 73856093u ^ y * 19349663u;
    }
};

// Authored cell -> level mapping (optional when cell_level_url_pattern is used).
struct WorldPartitionCellEntry
{
    DECLARE_SERIALIZE(WorldPartitionCellEntry)

    int32_t m_CoordX {0};
    int32_t m_CoordY {0};
    eastl::string m_LevelUrl;
};

template<typename TransferFunction>
void WorldPartitionCellEntry::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(m_CoordX, "coord_x");
    transfer.Transfer(m_CoordY, "coord_y");
    transfer.Transfer(m_LevelUrl, "level_url");
}

struct WorldPartitionSettings
{
    bool m_Enabled {false};
    bool m_AsyncCellLoading {true};
    float m_CellSize {12800.f};
    int32_t m_LoadingRange {1};
    Vector3 m_GridOrigin {};
    eastl::string m_CellLevelUrlPattern;
    std::vector<WorldPartitionCellEntry> m_Cells;
};

// World-space AABB for one cell (X-Y footprint, Z extent for debug draw only).
struct WorldPartitionCellBounds
{
    Vector3 m_Min {};
    Vector3 m_Max {};
};

void ComputeWorldPartitionCellBounds(const WorldPartitionSettings& settings,
                                     const WorldPartitionCellCoord& coord,
                                     WorldPartitionCellBounds& out_bounds);
