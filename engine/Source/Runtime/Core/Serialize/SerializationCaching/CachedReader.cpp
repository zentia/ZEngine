#include "CachedReader.h"

#include "CacheReaderBase.h"
#include "Runtime/Utility/MemoryUtility.h"

void CachedReader::InitRead(CacheReaderBase& cacher, size_t position, size_t readSize)
{
    m_Cacher = &cacher;
    m_CacheSize = m_Cacher->GetCacheSize();
    m_Block = position / m_CacheSize;
    m_MaxPosition = position + readSize;
    m_MinPosition = position;

    LockCacheBlockBounded();

    SetPosition(position);
}

size_t CachedReader::End()
{
    size_t position = GetPosition();

    m_Cacher->UnlockCacheBlock(m_Block);
    m_Block = -1;
    return position;
}

void CachedReader::UpdateReadCache(void* data, size_t size)
{
    size_t position = GetPosition();
    SetPosition(position);
    MemoryConstrainedSrc(data, m_CachePosition, size, m_CacheStart, m_CacheEnd);

    if (m_CachePosition + size > m_CacheEnd)
    {
        SetPosition(position + size);
        uint8_t* cachePosition = position - m_Block * m_CacheSize + m_CacheStart;
        MemoryConstrainedSrc(data, cachePosition, size, m_CacheStart, m_CacheEnd);
    }
    else
    {
        m_CachePosition += size;
    }
}

void CachedReader::LockCacheBlockBounded()
{
    m_Cacher->LockCacheBlock(m_Block, &m_CacheStart, &m_CacheEnd);
    uint8_t* maxPos = m_MaxPosition - m_Block * m_CacheSize + m_CacheStart;
    m_CacheEnd = std::min(m_CacheEnd, maxPos);
}

void CachedReader::Read(void* data, size_t size)
{
    if (m_CachePosition + size <= m_CacheEnd)
    {
        memcpy(data, m_CachePosition, size);
        m_CachePosition += size;
    }
    else
    {
        size_t position = GetPosition();

        if (position % m_CacheSize != 0)
        {
            size_t blockEnd = ((position / m_CacheSize) + 1) * m_CacheSize;
            size_t curReadSize = std::min<size_t>(size, blockEnd - position);
            MemoryConstrainedSrc(data, m_CachePosition, curReadSize, m_CacheStart, m_CacheEnd);
            m_CachePosition += curReadSize;
            position += curReadSize;
            (uint8_t*&)data += curReadSize;
            size -= curReadSize;
        }

        size_t physicallyLimitedSize = std::min<size_t>((position + size), m_Cacher->GetFileLength()) - position;
        size_t blocksToRead = physicallyLimitedSize / m_CacheSize;
        if (blocksToRead > 0)
        {
            size_t curReadSize = blocksToRead * m_CacheSize;
            m_Cacher->DirectRead(static_cast<uint8_t*>(data), position, curReadSize);
            m_CachePosition += curReadSize;
            (uint8_t*&)data += curReadSize;
            size -= curReadSize;
        }

        while (size != 0)
        {
            size_t curReadSize = std::min<size_t>(size, m_CacheSize);
            UpdateReadCache(data, curReadSize);
            (uint8_t*&)data += curReadSize;
            size -= curReadSize;
        }
    }
}

void CachedReader::SetPosition(size_t position)
{
    if (position / m_CacheSize != static_cast<size_t>(m_Block))
    {
        m_Cacher->UnlockCacheBlock(m_Block);
        m_Block = position / m_CacheSize;
        m_Cacher->LockCacheBlock(m_Block, &m_CacheStart, &m_CacheEnd);
    }
    m_CachePosition = position - m_Block * m_CacheSize + m_CacheStart;
}

void CachedReader::Align4Read()
{
    uint32_t offset = m_CachePosition - m_CacheStart;
    offset = ((offset + 3) >> 2) << 2;
    m_CachePosition = m_CacheStart + offset;
}