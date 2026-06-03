#pragma once

#include "CacheReaderBase.h"
#include "Runtime/File/AsyncReadManager.h"

class FileCacherRead : public CacheReaderBase
{
    struct CacheBlock
    {
        uint8_t* data;
        int block;
        int locked;
    };

public:
    FileCacherRead(const std::filesystem::path& path, size_t cacheSize);
    ~FileCacherRead();

    virtual void LockCacheBlock(size_t block, uint8_t** startPos, uint8_t** endPos);
    virtual void UnlockCacheBlock(size_t block);
    virtual void DirectRead(void* data, size_t position, size_t size);

    virtual size_t GetFileLength() const { return m_FileSize; }
    virtual size_t GetCacheSize() const { return m_CacheSize; }

private:
    int RequestBlock(int block);

    void AllocateBlock(CacheBlock& block, size_t sizeToUse);
    void DeallocateBlock(CacheBlock& block);

    bool Request(int block, int readCmdIndex, CacheBlock& cacheBlock, bool sync);
    void InitializeReadCommands(const std::function<void(CacheBlock& block)>& allocateBlock);
    enum
    {
        kCacheCount = 2
    };

    size_t m_CacheSize;
    size_t m_FileSize;
    std::filesystem::path m_Path;
    CacheBlock m_ActiveBlocks[kCacheCount];
    AsyncReadCommand m_ReadCommands[kCacheCount];
    AsyncReadCommand m_DirectReadCommands;
};