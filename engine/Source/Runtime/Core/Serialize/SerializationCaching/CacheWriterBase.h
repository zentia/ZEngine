#pragma once

class ICacheWriterBase
{
public:
    virtual bool CompleteWriting(size_t size) = 0;

    virtual void LockCacheBlock(size_t block, uint8_t** startPos, uint8_t** endPos) = 0;
    virtual void UnlockCacheBlock(size_t block) = 0;

    virtual void PreallocateForWrite(size_t block, uint8_t** startPos, uint8_t** endPos, size_t sizeOfWrite) {}

    virtual size_t GetCacheSize() = 0;

    virtual int DirectWrite(const void* data, size_t size) { return 0; }
    virtual bool SupportDirectWrite() const { return false; }
};