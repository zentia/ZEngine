#include "MemoryCacherReadBlocks.h"

#include "Runtime/Core/Serialize/SerializationCaching/CacheReaderBase.h"
#include "Runtime/VirtualFileSystem/VirtualFileSystemTypes.h"

BlockReadInfo CalculateBlockReadInfo(uint8_t* start, uint8_t* end, size_t offset, size_t size)
{
    const size_t blockReadSize = (end - start - offset);
    const size_t copyAmount = std::min(blockReadSize, size);
    const uint8_t* source = start + offset;
    return {source, copyAmount};
}

BlockRangeInfo CalculateBlockRange(size_t size, size_t position, size_t maxBlockSize)
{
    const size_t startBlock = position / maxBlockSize;
    const size_t offset = position % maxBlockSize;
    const size_t endBlock = ((position + size - 1) / maxBlockSize) + 1;

    return {startBlock, endBlock, offset};
}

void ReadFileCache(CacheReaderBase& cache, void* data, FileSize position, size_t size)
{
    const size_t cacheMaxBlockSize = cache.GetCacheSize();

    BlockRangeInfo range = CalculateBlockRange(size, position.Cast<size_t>(), cacheMaxBlockSize);

    uint8_t* dest = static_cast<uint8_t*>(data);

    for (size_t blockIdx = range.startIndex; blockIdx < range.endIndex; ++blockIdx)
    {
        uint8_t *blockStart, *blockEnd;
        cache.LockCacheBlock(blockIdx, &blockStart, &blockEnd);

        BlockReadInfo readInfo = CalculateBlockReadInfo(blockStart, blockEnd, range.offset, size);

        memcpy(dest, readInfo.source, readInfo.length);

        dest += readInfo.length;
        size -= readInfo.length;
        range.offset = 0;

        cache.UnlockCacheBlock(blockIdx);
    }
}
