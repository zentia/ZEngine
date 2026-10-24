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
            // ── 两阶段销毁（阶段一 #8）──────────────────────────────────────
            // 只对 Object 派生类型生效：写 ObjectFlags + 调 BeginDestroy/
            // FinishDestroy 钩子。用 if constexpr + is_base_of 约束，保证非
            // Object 的普通类型（这个模板同样会用来释放它们）不受影响、照常编译。
            // 必须在 ~T() 之前调用——此刻对象仍是最派生完整类型，虚钩子才能正确
            // 分发到子类 override。这是两条销毁入口（本函数 / ObjectManager::Destroy）
            // 收敛到同一实现的关键，否则 mimalloc 路径（NewObject 的主流）不会写 flags。
            if constexpr (std::is_base_of_v<Object, std::remove_pointer_t<T>>)
            {
                ptr->BeginDestroySequence();
            }

            // ── 分配器配对守卫（§3.26）────────────────────────────────────────
            // 本函数只能用来释放 mi_malloc 出来的对象（MemoryManager::CreateObject
            // / ObjectManager::NewObject）。ObjectManager::Produce() 走的是普通
            // `new`，必须配 ObjectManager::Destroy()。
            //
            // Apple 上全局 operator new/delete **故意不接管** mimalloc（见
            // MemoryManager.cpp 的 `#if !defined(__APPLE__)`），所以把 Produce 出来的
            // 指针喂进来会在 mi_usable_size → mi_checked_ptr_segment 里
            // EXC_BAD_ACCESS / SIGBUS —— 而且崩在释放路径上，栈里完全看不出是谁
            // 配错的。非 Apple 上 new 就是 mimalloc，同样的错配一直是隐形的。
            //
            // 所以这里先判一次"这块内存是不是 mimalloc 管的"：不是就明确报错并且
            // **不**交给 mi_free（宁可泄漏这一块，也不要崩在一个看不出根因的地方）。
            if (!IsMimallocOwned(ptr))
            {
                ReportForeignFree(ptr);
                ptr->~T();
                return;
            }
            ptr->~T();
            Free(ptr);
        }
    }

    /// `p` 是不是由 mimalloc 分配的。这是上面那条守卫的判据，也是冒烟测试用来
    /// 证明"两条分配路径在 Apple 上确实不同"的探针 —— 返回 false 就说明这块内存
    /// 来自系统 malloc（也就是 Produce/new），喂给 mi_free 必崩。
    ///
    /// 注意：非 Apple 上全局 new 就是 mimalloc，所以这里对 `new` 出来的指针也返回
    /// true —— 那条错配在那里既不崩、也检测不出来。这也是为什么这类 bug 只在
    /// Apple 上才暴露。
    static bool IsMimallocOwned(const void* p)
    {
        return p != nullptr && mi_is_in_heap_region(p);
    }

    /// 被守卫拦下的"用 mimalloc 释放非 mimalloc 内存"的次数。冒烟测试读它来确认
    /// 错配真的被检出（而不是静悄悄放过去了）。
    /// （下面两个都定义在 MemoryManager.cpp —— 避免在 header 里拉进日志头。）
    static uint32_t GetForeignFreeCount();
    static void     ReportForeignFree(const void* p);

    // NOTE: this runs on every deallocation, including frees issued by background
    // threads (asset scanning, streaming). Resolving LLMTracker through GET_SYSTEM on
    // that path used to touch the shared SystemRegistry type cache concurrently with
    // main-thread registration and fault inside strcmp (see crash_stack.txt). The
    // tracker pointer is therefore resolved once and cached; it stays valid for the
    // lifetime of the registry-owned system.
    static LLMTracker* CachedTracker()
    {
        // Deliberately does NOT cache a nullptr result: MemoryManager may be used
        // before LLMTracker is registered, and caching null then would disable LLM
        // tracking for the whole process.
        static LLMTracker* tracker = nullptr;
        if (tracker == nullptr)
        {
            tracker = GET_SYSTEM(LLMTracker);
        }
        return tracker;
    }

    static void* Malloc(size_t size)
    {
        void* ptr = mi_malloc(size);
        if (ptr != nullptr)
        {
            if (CachedTracker() != nullptr)
                CachedTracker()->TrackAllocation(ptr, size);
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
            if (CachedTracker() != nullptr)
                CachedTracker()->TrackDeallocation(ptr, size);
            mi_free(ptr);
        }
    }
};