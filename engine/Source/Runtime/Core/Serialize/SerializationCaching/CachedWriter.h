#pragma once
#include <stdint.h>

class ICacheWriterBase;
class FileSize;

class CachedWriter
{
public:
    void InitWrite(ICacheWriterBase& cacher);

    void PushWriter(ICacheWriterBase& cacher);
    void PopWriter();

    template<typename T>
    void Write(const T& data);

    void Write(const void* data, size_t size);

    void Align4Write();

    FileSize GetPosition() const;

    bool CompleteWriting();

private:
    struct ActiveWriter
    {
        uint8_t* cachePosition;
        uint8_t* cacheStart;
        uint8_t* cacheEnd;
        size_t block;
        ICacheWriterBase* cacheBase;
        bool directWrite;

        ActiveWriter()
        {
            cachePosition = nullptr;
            cacheStart = nullptr;
            cacheEnd = nullptr;
            block = std::numeric_limits<size_t>::max();
            cacheBase = nullptr;
            directWrite = false;
        }
        size_t GetPosition() const;
        inline size_t Remaining() const { return cacheEnd - cachePosition; }
    };

    ActiveWriter m_ActiveWriter;

    ActiveWriter m_BackupWriter;

    static void InitActiveWriter(CachedWriter::ActiveWriter& activeWriter, ICacheWriterBase& cacher);
    void SetPosition(FileSize position);
    void PreallocateForWrite(size_t sizeOfWrite);
};

template<typename T>
inline void CachedWriter::Write(const T& data)
{
    if (m_ActiveWriter.Remaining() >= sizeof(T))
    {
        StoreUnaligned(m_ActiveWriter.cachePosition, data);
        m_ActiveWriter.cachePosition += sizeof(T);
    }
    else
    {
        Write(&data, sizeof(data));
    }
}