#include "World.h"

#include "Runtime/Core/Serialize/SerializeUtility.h"

IMPLEMENT_REGISTER_CLASS(WorldRes)
IMPLEMENT_OBJECT_SERIALIZE(WorldRes)
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(WorldRes)

template<typename TransferFunction>
void WorldRes::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(name, "name");
    transfer.Transfer(m_LevelUrls, "level_urls");
    transfer.Transfer(m_DefaultLevelUrl, "default_level_url");
    transfer.Transfer(m_EnableWorldPartition, "enable_world_partition");
    transfer.Transfer(m_AsyncCellLoading, "async_cell_loading");
    transfer.Transfer(m_CellSize, "cell_size");
    transfer.Transfer(m_LoadingRange, "loading_range");
    transfer.Transfer(m_GridOrigin, "grid_origin");
    transfer.Transfer(m_CellLevelUrlPattern, "cell_level_url_pattern");
    transfer.Transfer(m_PartitionCells, "partition_cells");
}