#include "BufferedCacheWriter.h"

BufferedCacheWriter::BufferedCacheWriter(size_t cacheLimit)
    : m_LockCount(0),
      m_Size(0),
      m_CacheLimit(cacheLimit)
{
}

void BufferedCacheWriter::LockCacheBlock(size_t block, uint8_t** startPos, uint8_t** endPos)
{
    if (block)
    {
        FlushToDisk();
    }

    auto cacheInitialSize = std::min<size_t>(m_CacheLimit, kCacheInitialSize);
    if (m_Memory.size() < cacheInitialSize)
        m_Memory.resize(cacheInitialSize);

    *startPos = m_Memory.data();
    *endPos = m_Memory.data() + m_Memory.size();

    m_LockCount++;
}

void BufferedCacheWriter::UnlockCacheBlock(size_t block)
{
    m_LockCount--;
}

void BufferedCacheWriter::PreallocateForWrite(size_t block, uint8_t** startPos, uint8_t** endPos, size_t sizeOfWrite)
{
    auto cacheSize = std::min<size_t>(m_CacheLimit, kCacheSize);
    size_t newCacheSize = (m_Memory.size() + sizeOfWrite + cacheSize - 1) / cacheSize * cacheSize;
    newCacheSize = std::min<size_t>(newCacheSize, m_CacheLimit);

    if (newCacheSize <= m_Memory.size())
        return;

    if (newCacheSize < m_CacheLimit / 2)
        m_Memory.resize(newCacheSize);
    else
    {
        m_Memory.resize(m_CacheLimit);
    }

    *startPos = m_Memory.data();
    *endPos = m_Memory.data() + m_Memory.size();
}

bool BufferedCacheWriter::CompleteWriting(size_t size)
{
    m_Size = size;
    return true;
}

size_t BufferedCacheWriter::GetCacheSize()
{
    return std::max<size_t>(m_Memory.size(), std::min<size_t>(m_CacheLimit, kCacheInitialSize));
}

void BufferedCacheWriter::FlushToDisk()
{
    m_File.Write(m_Memory.data(), m_Memory.size());
}

void BufferedCacheWriter::WriteAndClose(CachedWriter& writer)
{
    writer.Write(m_Memory.data(), m_Size - m_BlockOnDisk * GetCacheSize());

    Reset();
}

void BufferedCacheWriter::Reset()
{
    m_Memory.clear();
}