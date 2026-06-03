#include "FileCacherWrite.h"

FileCacherWrite::FileCacherWrite()
{
    m_Success = true;
    m_CacheSize = 0;
    m_DataCache = nullptr;
}

bool FileCacherWrite::InitWriteFile(const std::filesystem::path& path, size_t cacheSize)
{
    m_Path = path;
    if (!m_File.Open(m_Path, kWritePermission, kNormalBehavior))
    {
        m_Success = false;
        return false;
    }

    m_Block = std::numeric_limits<size_t>::max();
    m_CacheSize = cacheSize;
    m_DataCache = static_cast<uint8_t*>(MemoryManager::Malloc(m_CacheSize));
    return true;
}

bool FileCacherWrite::CompleteWriting(size_t size)
{
    size_t remainingData = size - (m_Block * m_CacheSize);
    m_Success &= m_File.Write(static_cast<uint64_t>(m_Block) * m_CacheSize, m_DataCache, remainingData);
    m_Success &= m_File.Close();
    return m_Success;
}

FileCacherWrite::~FileCacherWrite()
{
    if (m_DataCache)
    {
        MemoryManager::Free(m_DataCache);
        m_DataCache = nullptr;
    }
    m_File.Close();
}

void FileCacherWrite::LockCacheBlock(size_t block, uint8_t** startPos, uint8_t** endPos)
{
    if (m_Block != block)
    {
        if (m_Block != std::numeric_limits<size_t>::max())
            m_Success &= m_File.Write(m_DataCache, m_CacheSize);

        m_Block = block;
    }

    *startPos = m_DataCache;
    *endPos = m_DataCache + m_CacheSize;
    m_Locked = true;
}

int FileCacherWrite::DirectWrite(const void* data, size_t size)
{
    int blockToWrite = size / m_CacheSize;
    if (blockToWrite)
    {
        m_File.Write(m_DataCache, m_CacheSize);
        m_File.Write(data, blockToWrite * m_CacheSize);
        m_Block += blockToWrite + 1;
    }
    return blockToWrite;
}