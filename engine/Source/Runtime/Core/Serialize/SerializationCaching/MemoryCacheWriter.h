#include "CacheWriterBase.h"

#include <vector>

static const size_t kShrinkToFitRatio = 8;

// =====================================================================================
// MemoryCacheWriter — ICacheWriterBase implementation that grows an external
// std::vector<uint8_t> as the writer streams data through it.
// -------------------------------------------------------------------------------------
// History:
//   The original version stored `m_Memory` BY VALUE, which silently dropped every
//   byte written through the cache (the caller's vector was never touched). It also
//   omitted the start/end pointer write-back inside `PreallocateForWrite`'s
//   "no growth needed" branch, leaving CachedWriter with stale (often null) cache
//   pointers. Both issues are fixed here.
//
// Contract (matches Unity's MemoryCacheWriter):
//   * Output buffer is the std::vector<uint8_t>& passed to the constructor.
//   * On Lock/Preallocate, we hand CachedWriter raw pointers into the vector's
//     storage. The vector may be grown to satisfy capacity requests.
//   * On CompleteWriting, the vector is resized down to the real payload size,
//     and (optionally) shrunk to fit if there is large slack.
//
// Caller responsibility:
//   The host must call `CachedWriter::CompleteWriting()` (or equivalent) before
//   reading the vector's size. Without that final call the vector still holds the
//   last preallocated capacity, not the logical write tip.
// =====================================================================================
class MemoryCacheWriter : public ICacheWriterBase
{
public:
    MemoryCacheWriter(std::vector<uint8_t>& mem)
        : m_Memory(mem), m_LockCount(0) {}

    virtual void LockCacheBlock(size_t block, uint8_t** startPos, uint8_t** endPos)
    {
        if (m_Memory.size() < kCacheInitialSize)
            m_Memory.resize(kCacheInitialSize);

        *startPos = m_Memory.data();
        *endPos = m_Memory.data() + m_Memory.size();

        m_LockCount++;
    }

    virtual void PreallocateForWrite(size_t block, uint8_t** startPos, uint8_t** endPos, size_t sizeOfWrite)
    {
        size_t newCacheSize = (m_Memory.size() + sizeOfWrite + kCacheSize - 1) / kCacheSize * kCacheSize;

        if (newCacheSize > m_Memory.size())
            m_Memory.resize(newCacheSize);

        // Always refresh start/end — even when no growth happened, the caller needs
        // the current pointers because it just transitioned from
        // `cacheStart=cacheEnd=nullptr` (post-construction) to a valid range, OR it
        // is recovering after a SetPosition/Push that invalidated them.
        *startPos = m_Memory.data();
        *endPos = m_Memory.data() + m_Memory.size();
    }

    virtual void UnlockCacheBlock(size_t block) override { m_LockCount--; }

    virtual bool CompleteWriting(size_t size) override
    {
        m_Memory.resize(size);

        if ((m_Memory.capacity() - size) > (size / kShrinkToFitRatio))
            m_Memory.shrink_to_fit();

        return true;
    }
    virtual size_t GetCacheSize() override { return m_Memory.size(); }

protected:
    enum
    {
        kCacheInitialSize = 256,
        kCacheSize = 4096,
    };
    // IMPORTANT: must be a reference. Holding by value would silently strand every
    // byte written through this writer (the caller's vector would never be updated).
    std::vector<uint8_t>& m_Memory;
    int32_t m_LockCount;
};
