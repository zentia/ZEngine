#pragma once

class CacheReaderBase
{
public:
    virtual void DirectRead(void* data, size_t position, size_t size) = 0;
    virtual void LockCacheBlock(size_t block, uint8_t** startPos, uint8_t** endPos) = 0;
    virtual void UnlockCacheBlock(size_t block) = 0;

    virtual size_t GetCacheSize() const = 0;
    virtual size_t GetFileLength() const = 0;
};