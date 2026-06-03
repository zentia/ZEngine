#pragma once
#include "Runtime/Utility/MemoryUtility.h"

#include <stdint.h>
class CacheReaderBase;

class CachedReader
{
public:
    void InitRead(CacheReaderBase& cacher, size_t position, size_t readSize);

    size_t End();

    template<typename T>
    void Read(T& data, size_t position)
    {
        m_CachePosition = m_CacheStart + position - m_Block * m_CacheSize;
        if (m_CachePosition >= m_CacheStart && m_CachePosition + sizeof(data) <= m_CacheEnd)
        {
            data = LoadUnaligned<T>(m_CachePosition);
            m_CachePosition += sizeof(T);
        }
        else
        {
            UpdateReadCache(&data, sizeof(data));
        }
    }

    template<typename T>
    void Read(T& data)
    {
        if (m_CachePosition + sizeof(T) <= m_CacheEnd)
        {
            data = LoadUnaligned<T>(m_CachePosition);
            m_CachePosition += sizeof(T);
        }
        else
        {
            UpdateReadCache(&data, sizeof(data));
        }
    }

    void Read(void* data, size_t size);

    void Align4Read();

    size_t GetPosition() const { return m_CachePosition - m_CacheStart + m_Block * m_CacheSize; }
    void SetPosition(size_t position);

private:
    void UpdateReadCache(void* data, size_t size);
    void LockCacheBlockBounded();
    uint8_t* m_CachePosition;
    uint8_t* m_CacheStart;
    uint8_t* m_CacheEnd;
    CacheReaderBase* m_Cacher;
    int32_t m_Block;
    size_t m_CacheSize;
    size_t m_MinPosition;
    size_t m_MaxPosition;
};