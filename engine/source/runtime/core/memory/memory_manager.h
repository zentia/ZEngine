#pragma once
#include <mimalloc.h>

namespace Z
{
    // engine/source/runtime/core/memory/memory_manager.h
    class MemoryManager
    {
    public:
        static void initialize();
        static void shutdown();

        template<typename T, typename... Args>
        static T* createObject(Args&&... args)
        {
            void* ptr = mi_malloc(sizeof(T));
            return new (ptr) T(std::forward<Args>(args)...);
        }

        template<typename T>
        static void destroyObject(T* ptr)
        {
            if (ptr)
            {
                ptr->~T();
                mi_free(ptr);
            }
        }

        static void* allocate(size_t size) { return mi_malloc(size); }

        static void deallocate(void* ptr) { mi_free(ptr); }
    };
} // namespace Z