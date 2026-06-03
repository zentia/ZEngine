#pragma once
#include "CacheWriterBase.h"
#include "Runtime/File/File.h"

class FileCacherWrite : public ICacheWriterBase
{
public:
    FileCacherWrite();
    bool InitWriteFile(const std::filesystem::path& path, size_t cacheSize);

    virtual ~FileCacherWrite();

    virtual void LockCacheBlock(size_t block, uint8_t** startPos, uint8_t** endPos) override;
    virtual void UnlockCacheBlock(size_t block) override { m_Locked = false; }
    virtual bool CompleteWriting(size_t size) override;

    virtual size_t GetCacheSize() override { return m_CacheSize; }

    virtual int DirectWrite(const void* data, size_t size) override;
    virtual bool SupportDirectWrite() const override { return true; }

private:
    size_t m_Block;
    uint8_t* m_DataCache;
    size_t m_CacheSize;

    File m_File;
    bool m_Success;
    bool m_Locked;
    std::filesystem::path m_Path;
};