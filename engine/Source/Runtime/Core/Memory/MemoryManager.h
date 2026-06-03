#pragma once
#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Core/Memory/LlmTracker.h"

#include <Core/Base/SystemRegistry.h>
#include <mimalloc.h>
#include <type_traits>

#define MEMORY_DELETE(p)             \
    MemoryManager::DestroyObject(p); \
    (p) = nullptr;

class MemoryManager : public IEngineSystem
{
public:
    std::string GetName() const override { return GET_CLASS_NAME(MemoryManager); }
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::PreInit; }
    std::vector<std::type_index> GetDependencies() const override;
    bool Initialize() override;
    void Shutdown() override;

    template<typename T, typename... Args>
    static T* CreateObject(Args&&... args)
    {
        void* ptr = Malloc(sizeof(T));
        T* obj = new (ptr) T(std::forward<Args>(args)...);

        if constexpr (std::is_base_of_v<Object, T>)
        {
            obj->InitializeRuntimeTypeInfo();
        }

        return obj;
    }

    template<typename T>
    static void DestroyObject(T* ptr)
    {
        if (ptr)
        {
            ptr->~T();
            Free(ptr);
        }
    }

    static void* Malloc(size_t size)
    {
        void* ptr = mi_malloc(size);
        if (ptr != nullptr)
        {
            if (GET_SYSTEM(LLMTracker) != nullptr)
                GET_SYSTEM(LLMTracker)->TrackAllocation(ptr, size);
        }
        return ptr;
    }

    static void* Realloc(void* ptr, size_t size) { return mi_realloc(ptr, size); }

    static void* ReallocAligned(void* data, size_t size, size_t align) { return mi_realloc_aligned(data, size, align); }

    static void Free(void* ptr)
    {
        if (ptr != nullptr)
        {
            // Get the size before freeing (mimalloc provides this)
            size_t size = mi_usable_size(ptr);
            if (GET_SYSTEM(LLMTracker) != nullptr)
                GET_SYSTEM(LLMTracker)->TrackDeallocation(ptr, size);
            mi_free(ptr);
        }
    }
};