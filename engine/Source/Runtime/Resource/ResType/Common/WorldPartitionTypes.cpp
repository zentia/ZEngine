#include "WorldPartitionTypes.h"

void ComputeWorldPartitionCellBounds(const WorldPartitionSettings& settings,
                                     const WorldPartitionCellCoord& coord,
                                     WorldPartitionCellBounds& out_bounds)
{
    const float min_x = settings.m_GridOrigin.x + static_cast<float>(coord.m_X) * settings.m_CellSize;
    const float min_y = settings.m_GridOrigin.y + static_cast<float>(coord.m_Y) * settings.m_CellSize;
    const float max_x = min_x + settings.m_CellSize;
    const float max_y = min_y + settings.m_CellSize;

    out_bounds.m_Min = Vector3(min_x, min_y, settings.m_GridOrigin.z);
    // Debug/visualization height only (gameplay bounds are the X-Y footprint).
    out_bounds.m_Max = Vector3(max_x, max_y, settings.m_GridOrigin.z + settings.m_CellSize);
}
