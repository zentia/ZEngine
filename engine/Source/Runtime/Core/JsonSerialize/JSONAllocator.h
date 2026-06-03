#pragma once
#include "Runtime/Core/Memory/MemoryManager.h"
class JSONAllocator
{
public:
    static const bool kNeedFree = true;
    void* Malloc(size_t size) { return MemoryManager::Malloc(size); }
    void* Realloc(void* originalPtr, size_t originalSize, size_t newSize)
    {
        return MemoryManager::Realloc(originalPtr, newSize);
    }
    static void Free(void* ptr) { MemoryManager::Free(ptr); }
};