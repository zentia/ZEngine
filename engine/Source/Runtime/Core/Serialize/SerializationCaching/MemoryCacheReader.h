#pragma once

#include "CacheReaderBase.h"

// =====================================================================================
// MemoryCacheReader — read-side counterpart of MemoryCacheWriter.
// -------------------------------------------------------------------------------------
// IMPORTANT: m_Memory is held by REFERENCE, mirroring the fix applied to
// MemoryCacheWriter in Phase 2b-1.
//
// Background — original bug:
//   The previous implementation stored `std::vector<uint8_t> m_Memory` by value, which
//   *copied* the caller-provided buffer at construction. That was harmless for the
//   reader path because reads can't invalidate the caller's view — but it cost a full
//   buffer copy on every WriteObjectToVector → MemoryCacheReader round-trip, and it
//   created an asymmetry with MemoryCacheWriter (which now correctly uses a reference).
//
// We therefore standardise on by-reference here too: zero-copy round-trip and an
// explicit lifetime contract — the caller must keep the underlying vector alive for
// the lifetime of the reader.
// =====================================================================================
class MemoryCacheReader : public CacheReaderBase
{
public:
    MemoryCacheReader(std::vector<uint8_t>& mem)
        : m_Memory(mem), m_LockCount(0)
    {
    }

    virtual void LockCacheBlock(size_t block, uint8_t** startPos, uint8_t** endPos)
    {
        *startPos = m_Memory.size() > block * kCacheSize ? &m_Memory[block * kCacheSize] : nullptr;
        *endPos = *startPos + std::min<size_t>((size_t)(GetFileLength() - block * kCacheSize), kCacheSize);
        m_LockCount++;
    }

    virtual void DirectRead(void* data, size_t position, size_t size)
    {
        memcpy(data, &m_Memory[position], size);
    }

    virtual void UnlockCacheBlock([[maybe_unused]] size_t block) { m_LockCount--; }

    virtual size_t GetFileLength() const { return m_Memory.size(); }
    virtual size_t GetCacheSize() const { return kCacheSize; }

protected:
    enum
    {
        kCacheSize = 256
    };

    /// Backing store for the readable buffer. Held by reference — caller owns the
    /// storage and must outlive this reader. Symmetric with MemoryCacheWriter's
    /// `std::vector<uint8_t>& m_Memory`.
    std::vector<uint8_t>& m_Memory;

    int32_t m_LockCount;
};
