#include "CachedWriter.h"

#include "CacheWriterBase.h"
#include "Runtime/Utility/Align.h"

void CachedWriter::InitWrite(ICacheWriterBase& cacher)
{
    InitActiveWriter(m_ActiveWriter, cacher);
}

void CachedWriter::PushWriter(ICacheWriterBase& cacher)
{
    ActiveWriter& activeWriter = m_ActiveWriter;

    m_BackupWriter = activeWriter;
    activeWriter.block = -1;

    InitActiveWriter(activeWriter, cacher);
}

void CachedWriter::PopWriter()
{
    ActiveWriter& activeWriter = m_ActiveWriter;

    activeWriter.cacheBase->UnlockCacheBlock(activeWriter.block);
    activeWriter.cacheBase->CompleteWriting(activeWriter.GetPosition());

    m_ActiveWriter = m_BackupWriter;

    m_BackupWriter.block = -1;
}

void CachedWriter::Write(const void* data, size_t size)
{
    if (!size)
        return;

    if (size > m_ActiveWriter.Remaining())
        PreallocateForWrite(size - m_ActiveWriter.Remaining());

    while (size)
    {
        if (size_t toCopy = std::min<size_t>(m_ActiveWriter.Remaining(), size))
        {
            memcpy(m_ActiveWriter.cachePosition, data, toCopy);
            m_ActiveWriter.cachePosition += toCopy;
            (uint8_t*&)data += toCopy;
            size -= toCopy;
        }

        if (size)
        {
            if (m_ActiveWriter.directWrite && size > m_ActiveWriter.cacheBase->GetCacheSize())
            {
                m_ActiveWriter.cacheBase->UnlockCacheBlock(m_ActiveWriter.block);

                int blockWritten = m_ActiveWriter.cacheBase->DirectWrite(data, size);

                int copied = blockWritten * m_ActiveWriter.cacheBase->GetCacheSize();
                (uint8_t*&)data += copied;
                size -= copied;

                m_ActiveWriter.block += blockWritten + 1;
                m_ActiveWriter.cacheBase->LockCacheBlock(m_ActiveWriter.block, &m_ActiveWriter.cacheStart, &m_ActiveWriter.cacheEnd);
                m_ActiveWriter.cachePosition = m_ActiveWriter.cacheStart;
            }
            else
            {
                SetPosition(GetPosition());

                if (!m_ActiveWriter.Remaining())
                {
                    break;
                }
            }
        }
    }
}

void CachedWriter::Align4Write()
{
    if (uint32_t leftOver = Align4LeftOver(m_ActiveWriter.cachePosition - m_ActiveWriter.cacheStart))
    {
        uint32_t value = 0;
        Write(&value, leftOver);
    }
}

void CachedWriter::InitActiveWriter(ActiveWriter& activeWriter, ICacheWriterBase& cacher)
{
    activeWriter.cacheBase = &cacher;
    activeWriter.block = 0;
    activeWriter.cacheBase->LockCacheBlock(activeWriter.block, &activeWriter.cacheStart, &activeWriter.cacheEnd);
    activeWriter.cachePosition = activeWriter.cacheStart;
    activeWriter.directWrite = cacher.SupportDirectWrite();
}

void CachedWriter::PreallocateForWrite(size_t sizeOfWrite)
{
    size_t position = GetPosition().Cast<size_t>();
    m_ActiveWriter.cacheBase->PreallocateForWrite(m_ActiveWriter.block, &m_ActiveWriter.cacheStart, &m_ActiveWriter.cacheEnd, sizeOfWrite);
    m_ActiveWriter.cachePosition = position - m_ActiveWriter.block * m_ActiveWriter.cacheBase->GetCacheSize() + m_ActiveWriter.cacheStart;
}

void CachedWriter::SetPosition(FileSize position)
{
    size_t newBlock = FileSize(position / (uint64_t)m_ActiveWriter.cacheBase->GetCacheSize()).Cast<size_t>();
    if (newBlock != m_ActiveWriter.block)
    {
        m_ActiveWriter.cacheBase->UnlockCacheBlock(m_ActiveWriter.block);
        m_ActiveWriter.cacheBase->LockCacheBlock(newBlock, &m_ActiveWriter.cacheStart, &m_ActiveWriter.cacheEnd);

        m_ActiveWriter.block = FileSize(position / (uint64_t)m_ActiveWriter.cacheBase->GetCacheSize()).Cast<size_t>();
    }
    m_ActiveWriter.cachePosition = m_ActiveWriter.cacheStart + (position.Cast<size_t>() % m_ActiveWriter.cacheBase->GetCacheSize());
}

size_t CachedWriter::ActiveWriter::GetPosition() const
{
    return cachePosition - cacheStart + block * cacheBase->GetCacheSize();
}

FileSize CachedWriter::GetPosition() const
{
    return static_cast<uint64_t>(m_ActiveWriter.GetPosition());
}

bool CachedWriter::CompleteWriting()
{
    m_ActiveWriter.cacheBase->UnlockCacheBlock(m_ActiveWriter.block);

    return m_ActiveWriter.cacheBase->CompleteWriting(m_ActiveWriter.GetPosition());
}