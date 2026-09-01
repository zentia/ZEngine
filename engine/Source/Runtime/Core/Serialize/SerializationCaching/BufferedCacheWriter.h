#pragma once
#include "CacheWriterBase.h"
#include "Runtime/File/File.h"

#include <filesystem>
#include <vector>

class CachedWriter;

class BufferedCacheWriter : public ICacheWriterBase
{
public:
    enum
    {
        kCacheInitialSize = 256,
        kCacheSize = 4096,
        kCacheLimit = 256 * 1024 * 1024
    };

    BufferedCacheWriter(size_t cacheLimit = kCacheLimit);

    void LockCacheBlock(size_t block, uint8_t** startPos, uint8_t** endPos) override;
    void UnlockCacheBlock(size_t block) override;

    virtual void PreallocateForWrite(size_t block, uint8_t** startPos, uint8_t** endPos, size_t sizeOfWrite) override;

    bool CompleteWriting(size_t size) override;

    void WriteAndClose(CachedWriter& writer);

    size_t GetCacheSize() override;

    void Reset();

    inline size_t Size() const { return m_Size; }

protected:
    void FlushToDisk();

    std::vector<uint8_t> m_Memory;
    int32_t m_LockCount;
    size_t m_BlockOnDisk;
    size_t m_Size;
    File m_File;
    std::filesystem::path m_TempPath;
    size_t m_CacheLimit;
};