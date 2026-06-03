#pragma once
#include <stdint.h>
class FileSize;
struct BlockReadInfo
{
    const uint8_t* source;
    size_t length;
};

struct BlockRangeInfo
{
    size_t startIndex;
    size_t endIndex;
    size_t offset;
};

void ReadFileCache(CacheReaderBase& cacher, void* data, FileSize postiion, size_t size);