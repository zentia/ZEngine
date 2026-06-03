#include "WorldPartitionEditorDebug.h"

#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Math/Vector4.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "Runtime/Function/Render/DebugDraw/DebugDrawGroup.h"
#include "Runtime/Function/Render/DebugDraw/DebugDrawManager.h"
#include "Runtime/Function/Render/DebugDraw/DebugDrawPrimitive.h"
#include "Runtime/Resource/ResType/Common/WorldPartitionTypes.h"

namespace
{
    void DrawCellBounds(DebugDrawGroup& group,
                        const WorldPartitionSettings& settings,
                        const WorldPartitionCellCoord& coord,
                        const Vector4& color)
    {
        WorldPartitionCellBounds bounds;
        ComputeWorldPartitionCellBounds(settings, coord, bounds);

        const Vector3 corners[4] = {
            Vector3(bounds.m_Min.x, bounds.m_Min.y, bounds.m_Min.z),
            Vector3(bounds.m_Max.x, bounds.m_Min.y, bounds.m_Min.z),
            Vector3(bounds.m_Max.x, bounds.m_Max.y, bounds.m_Min.z),
            Vector3(bounds.m_Min.x, bounds.m_Max.y, bounds.m_Min.z),
        };

        for (int edge = 0; edge < 4; ++edge)
        {
            const int next = (edge + 1) % 4;
            group.AddLine(corners[edge], corners[next], color, color, k_debug_draw_one_frame, true);
        }
    }
}  // namespace

void DrawWorldPartitionEditorOverlay(const Vector3& streaming_source)
{
    auto world = GET_SYSTEM(WorldManager);
    if (!world || !world->IsWorldPartitionEnabled())
    {
        return;
    }

    auto debug_draw = GET_SYSTEM(DebugDrawManager);
    if (!debug_draw)
    {
        return;
    }

    DebugDrawGroup* group = debug_draw->TryGetOrCreateDebugDrawGroup("WorldPartition");
    if (!group)
    {
        return;
    }

    const WorldPartition& partition = world->GetWorldPartition();
    const WorldPartitionSettings& settings = partition.GetSettings();

    std::vector<WorldPartitionCellCoord> loaded;
    std::vector<WorldPartitionCellCoord> pending;
    std::vector<WorldPartitionCellCoord> desired;
    partition.CollectCellsForDebug(streaming_source, loaded, pending, desired);

    const Vector4 loaded_color(0.2f, 1.0f, 0.3f, 1.0f);
    const Vector4 pending_color(1.0f, 0.85f, 0.1f, 1.0f);
    const Vector4 desired_color(0.3f, 0.7f, 1.0f, 0.85f);

    for (const WorldPartitionCellCoord& coord : loaded)
    {
        DrawCellBounds(*group, settings, coord, loaded_color);
    }
    for (const WorldPartitionCellCoord& coord : pending)
    {
        DrawCellBounds(*group, settings, coord, pending_color);
    }
    for (const WorldPartitionCellCoord& coord : desired)
    {
        bool already_drawn = false;
        for (const WorldPartitionCellCoord& loaded_coord : loaded)
        {
            if (loaded_coord == coord)
            {
                already_drawn = true;
                break;
            }
        }
        if (!already_drawn)
        {
            for (const WorldPartitionCellCoord& pending_coord : pending)
            {
                if (pending_coord == coord)
                {
                    already_drawn = true;
                    break;
                }
            }
        }
        if (!already_drawn)
        {
            DrawCellBounds(*group, settings, coord, desired_color);
        }
    }
}
