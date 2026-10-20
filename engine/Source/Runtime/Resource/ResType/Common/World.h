#pragma once
#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Resource/ResType/Common/WorldPartitionTypes.h"

#include <vector>
#include "World.generated.h"

ZCLASS(CustomTransfer)
class WorldRes : public Object
{
    ZGENERATED_BODY(WorldRes)

public:
    // all level urls for this world
    std::vector<eastl::string> m_LevelUrls;

    // the default level for this world, which should be first loading level
    eastl::string m_DefaultLevelUrl;

    // UE World Partition (optional). When enabled, WorldManager streams Level cells on the X-Y grid.
    bool m_EnableWorldPartition {false};
    bool m_AsyncCellLoading {true};
    float m_CellSize {12800.f};
    int32_t m_LoadingRange {1};
    Vector3 m_GridOrigin {};
    eastl::string m_CellLevelUrlPattern;
    std::vector<WorldPartitionCellEntry> m_PartitionCells;
};
