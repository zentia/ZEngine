#include "BufferedCacheWriter.h"

#include "CachedWriter.h"

#include <atomic>

BufferedCacheWriter::BufferedCacheWriter(size_t cacheLimit)
    : m_LockCount(0),
      m_BlockOnDisk(0),
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
    // Unity BufferedCacheWriter::FlushToDisk (Runtime/Serialize/SerializationCaching/
    // BufferedCacheWriter.cpp): the memory buffer is reused across blocks; once it
    // overflows kCacheLimit the full block is spilled to a temporary file. The
    // temp file is opened lazily on the first flush -- this Open() was missing in
    // ZEngine, so File::Write dereferenced a null FileAccessor (see the profiler
    // save crash: FileAccessor::Write -> File::Write -> FlushToDisk).
    if (!m_BlockOnDisk)
    {
        static std::atomic<uint64_t> tempFileCounter{0};
        const uint64_t id = tempFileCounter.fetch_add(1, std::memory_order_relaxed);
        m_TempPath = std::filesystem::temp_directory_path() /
                     ("zengine_serialize_" + std::to_string(id) + ".tmp");
        m_File.Open(m_TempPath, kReadWritePermission, kNormalBehavior);
    }

    m_BlockOnDisk++;
    m_File.Write(m_Memory.data(), m_Memory.size());
}

void BufferedCacheWriter::WriteAndClose(CachedWriter& writer)
{
    if (m_BlockOnDisk)
    {
        const size_t cacheSize = GetCacheSize();
        const size_t totalOnDisk = m_BlockOnDisk * cacheSize;

        std::vector<uint8_t> buffer(kCacheSize);
        size_t remaining = totalOnDisk;
        uint64_t offset = 0;
        while (remaining)
        {
            const size_t chunk = std::min<size_t>(remaining, buffer.size());
            const size_t read = m_File.Read(FileSize(offset), buffer.data(), chunk);
            writer.Write(buffer.data(), read);
            offset += static_cast<uint64_t>(read);
            remaining -= read;
        }
    }

    writer.Write(m_Memory.data(), m_Size - m_BlockOnDisk * GetCacheSize());

    Reset();
}

void BufferedCacheWriter::Reset()
{
    if (m_BlockOnDisk)
    {
        m_File.Close();
        std::error_code ec;
        std::filesystem::remove(m_TempPath, ec);
        m_BlockOnDisk = 0;
    }

    m_Memory.clear();
}