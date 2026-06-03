#include "MemoryManager.h"

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>

// =============================================================================
// 全局 operator new/delete 策略
// -----------------------------------------------------------------------------
// 在 iOS / macOS 这类"作为动态库被 host 进程集成"的场景下，dylib 与 host 进程
// 的 libc++ 共享同一份 std::string / std::filesystem::path 类型，但若我们在
// dylib 内重载了全局 operator new/delete 让其走 mimalloc，就会出现：
//   * 在 dylib 内部分配 (mimalloc) 的指针被 host 端释放 (system free)，或反之；
//   * libc++ 的 inline 函数 (例如 std::string::__move_assign) 在 dylib 内被实
//     例化时 deallocate 路由到 mimalloc，但该 buffer 实际是 host 用 system
//     malloc 分配的（参考 ProfilerRuntime::SetSavePath 触发的崩溃栈：
//       mi_free_block_delayed_mt -> mi_free -> operator delete (mimalloc) ->
//       std::string::__move_assign）。
// 因此在 Apple 平台只让 mimalloc 作为内部显式 mi_malloc/mi_free 使用 (见
// MemoryManager::Malloc/Free 与 AutoMemoryTyped)，绝不全局接管 new/delete。
// 其它平台保持原有的"DLL 内部一致用 mimalloc"策略。
// =============================================================================
#if !defined(__APPLE__)
    // mimalloc 官方 override：覆盖全局 operator new/delete，必须在仅此一个 .cpp 中包含
    // 确保优先于标准库，避免 allocator 混用
    #include <mimalloc-new-delete.h>
#endif

// =============================================================================
// EASTL allocator: placement new[] with debug params
// EASTL 默认 allocator 会调用这些带 debug 形参的 placement new[]/delete[]。
// 必须保证 new[]/delete[] 用同一个分配器，否则会崩。
// 在 Apple 平台走系统 malloc/free，其它平台走 mimalloc，与全局 new/delete 保持
// 一致。
// =============================================================================
#if defined(__APPLE__)
    #define ZENGINE_EASTL_MALLOC(sz)            std::malloc((sz))
    #define ZENGINE_EASTL_MALLOC_ALIGNED(sz, a) ::mi_malloc_aligned((sz), (a))  // fallback below
    #define ZENGINE_EASTL_FREE(p)               std::free((p))
#else
    #define ZENGINE_EASTL_MALLOC(sz)            mi_malloc((sz))
    #define ZENGINE_EASTL_MALLOC_ALIGNED(sz, a) mi_malloc_aligned((sz), (a))
    #define ZENGINE_EASTL_FREE(p)               mi_free((p))
#endif

#if defined(__APPLE__)
// Apple: posix_memalign 提供对齐分配，配套 std::free 释放
static void* zengine_aligned_malloc(size_t size, size_t alignment)
{
    if (alignment <= alignof(std::max_align_t))
        return std::malloc(size);
    void* p = nullptr;
    if (posix_memalign(&p, alignment < sizeof(void*) ? sizeof(void*) : alignment, size) != 0)
        return nullptr;
    return p;
}
    #undef ZENGINE_EASTL_MALLOC_ALIGNED
    #define ZENGINE_EASTL_MALLOC_ALIGNED(sz, a) zengine_aligned_malloc((sz), (a))
#endif

void* operator new[](size_t size, const char* /*pName*/, int /*flags*/, unsigned int /*align*/, const char* /*file*/, int /*line*/)
{
    return ZENGINE_EASTL_MALLOC(size);
}

void* operator new[](size_t size, size_t alignment, size_t offset, const char* /*pName*/, int /*flags*/, unsigned int /*align*/, const char* /*file*/, int /*line*/)
{
    (void)offset;  // EASTL deallocate expects the returned pointer to be freed directly; offset != 0 would require storing original ptr
    if (alignment <= 1)
        return ZENGINE_EASTL_MALLOC(size);
    return ZENGINE_EASTL_MALLOC_ALIGNED(size, alignment);
}

#if defined(__APPLE__)
// Apple 上没有重载全局 operator delete[](void*)，由系统 libc++ 提供（走 system free）。
// 这两个带 debug 形参的 EASTL 配套 delete 直接走 system free 即可。
void operator delete[](void* p, const char* /*pName*/, int /*flags*/, unsigned int /*align*/, const char* /*file*/, int /*line*/) noexcept
{
    std::free(p);
}

void operator delete[](void* p, size_t /*alignment*/, size_t /*offset*/, const char* /*pName*/, int /*flags*/, unsigned int /*align*/, const char* /*file*/, int /*line*/) noexcept
{
    std::free(p);
}
#else
// operator delete[](void* p) 已由 mimalloc-new-delete.h 提供
void operator delete[](void* p, const char* /*pName*/, int /*flags*/, unsigned int /*align*/, const char* /*file*/, int /*line*/) noexcept
{
    mi_free(p);
}

void operator delete[](void* p, size_t /*alignment*/, size_t /*offset*/, const char* /*pName*/, int /*flags*/, unsigned int /*align*/, const char* /*file*/, int /*line*/) noexcept
{
    mi_free(p);
}
#endif

// 注意：
// - 非 Apple 平台：mimalloc-new-delete.h 已提供 operator new/delete/new[]/delete[]
//   的默认重载；上面的 EASTL placement new 重载是额外的（用于 EASTL 的 debug allocator）。
// - Apple 平台：全局 operator new/delete 走系统 libc++（默认 system malloc/free）；
//   mimalloc 仅通过 MemoryManager::Malloc/Free 与 mi_* 显式调用使用。

// =============================================================================
// EA::StdC::Vsnprintf (required by EASTL string)
// =============================================================================
namespace EA
{
    namespace StdC
    {
        int Vsnprintf(char* __restrict pDestination, size_t n, const char* __restrict pFormat, va_list arguments)
        {
            return vsnprintf(pDestination, n, pFormat, arguments);
        }
    }  // namespace StdC
}  // namespace EA

// =============================================================================
// MemoryManager
// =============================================================================

std::vector<std::type_index> MemoryManager::GetDependencies() const
{
    return {};
}

bool MemoryManager::Initialize()
{
    LOG_INFO(ZEngine, __FUNCTION__);
    return true;
}

void MemoryManager::Shutdown() {}
