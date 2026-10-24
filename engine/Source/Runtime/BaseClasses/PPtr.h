#pragma once

#include "IPPtrResolver.h"
#include "LocalSerializedObjectIdentifier.h"
#include "Object.h"
#include "ObjectManager.h"

// UafReporter 只在运行期检测分支（ZENGINE_LIFETIME_CHECK）下才被 operator T*()
// 用到，但它是自由函数、声明轻量（不拖 ObjectManager / Object 实现），直接 include
// 无副作用。见 operator T*() 里"已销毁堆对象 vs 未解析磁盘对象"的区分。
#include "Runtime/Core/Memory/UafReporter.h"

#include <stdint.h>

// ZENGINE_LIFETIME_CHECK 由 CMake 注入（Debug/DebugV8/RelWithDebInfo=1，
// Release/MinSizeRel=0）。PPtr.h 被极广泛地 include（res_type / Component 等），
// 若某个 TU 脱离主工程树单独编译、未经 CMake 注入，这里给一个自包含的兜底：
// 默认关闭检测，operator T*() 的解析路径与改造前完全一致（零开销、不报告）。
#ifndef ZENGINE_LIFETIME_CHECK
    #define ZENGINE_LIFETIME_CHECK 0
#endif

template<typename T>
class PPtr
{
public:
    inline void AssignObject(const Object* o);

    static const char* GetTypeString();
    static bool AllowTransferOptimization() { return false; }

    template<typename TransferFunction>
    void Transfer(TransferFunction& transfer);

    PPtr()
        : m_InstanceID(0) {}

    explicit PPtr(int32_t instanceID) { m_InstanceID = instanceID; }
    PPtr(const T* o) { AssignObject(o); }

    template<typename U>
    PPtr<T>(const PPtr<U>& o)
    {
        m_InstanceID = o.GetInstanceID();
    }

    PPtr& operator=(const T* o)
    {
        AssignObject(o);
        return *this;
    }

    template<typename U>
    PPtr& operator=(const PPtr<U>& o)
    {
        m_InstanceID = o.GetInstanceID();
        return *this;
    }

    operator T*() const;
    T* operator->() const;

    int32_t GetInstanceID() const { return m_InstanceID; }
    bool IsNull() const;

private:
    int32_t m_InstanceID;
};

template<typename T>
inline void PPtr<T>::AssignObject(const Object* o)
{
    if (o == nullptr)
        m_InstanceID = 0;
    else
        m_InstanceID = o->GetInstanceID();
}

template<typename T>
inline const char* PPtr<T>::GetTypeString()
{
    return T::GetPPtrTypeString();
}

template<typename T>
PPtr<T>::operator T*() const
{
    if (m_InstanceID == 0)
        return nullptr;
    Object* temp = GET_SYSTEM(ObjectManager)->IDToPointer(m_InstanceID);
    if (temp == nullptr)
    {
        // IDToPointer 返回 null 有两种截然不同的含义，绝不能混为一谈：
        //
        //   (1) **堆对象已销毁**（instanceID 为负）：ObjectManager 给堆对象分配
        //       单调递减的负 ID（见 ObjectManager.h 的 m_NextHeapInstanceID 从 -1
        //       起），~Object() 销毁时把该负 ID 从表里注销。此刻这个 PPtr 指向的
        //       是一个**已经析构、内存可能已归还/复用**的对象——继续解析它就是在
        //       做 use-after-free。过去这里会无条件掉进 ReadObjectFromPersistentManager，
        //       恰好因为 remapper 里没有负 ID 才"碰巧"返回 null，把 UAF 静默掩盖成
        //       一次磁盘查询。这正是 PHASE1_FOUNDATION_PLAN.md #6 要治的隐患：
        //       比返回 null 更危险的是把"已销毁"悄悄升级成"去磁盘加载同 ID 对象"。
        //
        //   (2) **磁盘对象尚未加载**（instanceID 为正）：AssetManager 的 remapper
        //       给持久化对象分配单调递增的正 ID（见 Remapper.cpp 的 IncreaseHighestInstanceID）。
        //       一个正 ID 查不到 = 该 asset 还没被 hydrate，PPtr 的惰性加载契约就是要
        //       把它从磁盘 reify 出来——这条路径必须保留，不能因为"查不到"就返回 null。
        //
        // 用 ID 的符号区分二者：负 = 堆（已销毁即 UAF），正 = 磁盘（未解析即惰性加载）。
        // 与 ImmediatePtr 的判据（UafReporter 用 IsInstanceAlive 查表）互补：ImmediatePtr
        // 只认"死没死"，PPtr 还要区分"死在堆上"还是"根本没在堆上出现过"。
        //
        // 关键：负 ID 未命中 = 堆对象已销毁，**返回 null 本身就是修复**（不能掉进磁盘
        // reify）。这一条在所有配置下都成立，不 gate 在 ZENGINE_LIFETIME_CHECK 上——
        // 否则 Release 下会退回到改造前的老路（掉进 ReadObjectFromPersistentManager，
        // 对一个负 ID 做 remapper 查询，行为未定义、实测会崩）。只有"报告"（断言 + 日志）
        // 才是 Debug 专属的开销，用宏 gate 掉以维持 Release 零开销。
        if (m_InstanceID < 0)
        {
#if ZENGINE_LIFETIME_CHECK
            // 堆对象已销毁仍被解引用 = use-after-free。报告（Debug 下 ASSERT 中断），
            // 然后统一返回 null——不再掉进磁盘 reify。
            zengine::lifetime::ReportUseAfterFree(m_InstanceID, "PPtr<T>::operator T*");
#endif
            return nullptr;
        }
        temp = ReadObjectFromPersistentManager(m_InstanceID);
    }
    return static_cast<T*>(temp);
}

template<typename T>
inline T* PPtr<T>::operator->() const
{
    // Delegate to operator T*() so both go through the SAME resolution path.
    // operator T*() handles three cases this used to conflate:
    //   * m_InstanceID == 0  -> null (no identity).
    //   * negative instanceID -> a heap object; if IDToPointer misses, it was
    //     **already destroyed** (UAF), so report + return null instead of
    //     silently reifying something from disk (see operator T*()).
    //   * positive instanceID -> a not-yet-loaded disk object; miss means
    //     lazy-load it from the persistent manager. This is the property that
    //     lets `pptr->field` reify an asset that isn't resident yet, while
    //     `static_cast<T*>(pptr)` on the very same PPtr resolves identically.
    // Without this split, Skeleton::ApplyAnimation ended up calling
    // std::vector::size() on a null AnimSkelMap because a destroyed heap PPtr
    // was being laundered through a disk lookup that happened to return null.
    return operator T*();
}

template<typename T>
inline bool PPtr<T>::IsNull() const
{
    T* casted = *this;
    return casted == nullptr;
}

template<typename T>
template<typename TransferFunction>
inline void PPtr<T>::Transfer(TransferFunction& transfer)
{
    // Wire-format contract (Unity-1:1):
    //   m_FileID : int32  -- index into the owning SerializedFile's
    //                        externals table (0 = self / local target)
    //   m_PathID : int64  -- LocalIdentifierInFile of the target
    //
    // Read/Write distinguishes only on which side talks to the
    // IPPtrResolver: writers turn the engine-side InstanceID into an
    // LSOI BEFORE the field transfer; readers transfer the LSOI first
    // and then ask the resolver to translate it AFTER. The two field
    // Transfer() calls themselves are direction-agnostic and produce a
    // TypeTree of {m_FileID:int32, m_PathID:int64} via
    // GenerateTypeTreeTransfer for free.
    LocalSerializedObjectIdentifier lsoi;
    lsoi.localSerializedFileIndex = 0;
    lsoi.localIdentifierInFile = 0;

    IPPtrResolver* resolver = GetCurrentPPtrResolver();

    if constexpr (TransferFunction::IsWriting())
    {
        // Engine -> disk. Ask resolver to map InstanceID into (FileID,
        // PathID); resolver also internally registers any newly-seen
        // external file. If no resolver is active we fall back to a
        // raw round-trip: write the InstanceID into m_PathID and 0
        // into m_FileID. This keeps in-memory transfer paths
        // (clipboard, undo, prefab apply pre-serialization) working
        // exactly like before this PR.
        if (resolver != nullptr)
        {
            resolver->InstanceIDToLSOI(m_InstanceID, lsoi);
        }
        else
        {
            lsoi.localSerializedFileIndex = 0;
            lsoi.localIdentifierInFile = static_cast<int64_t>(m_InstanceID);
        }
    }

    int32_t fileID = lsoi.localSerializedFileIndex;
    int64_t pathID = lsoi.localIdentifierInFile;
    transfer.Transfer(fileID, "m_FileID");
    transfer.Transfer(pathID, "m_PathID");

    if constexpr (TransferFunction::IsReading())
    {
        // Disk -> engine. Resolver translates the on-disk LSOI back
        // into a runtime InstanceID; missing resolver triggers the
        // same raw fallback as the writer side, which lets in-memory
        // round-trips stay byte-exact.
        lsoi.localSerializedFileIndex = fileID;
        lsoi.localIdentifierInFile = pathID;

        if (resolver != nullptr)
        {
            m_InstanceID = resolver->LSOIToInstanceID(lsoi);
        }
        else
        {
            m_InstanceID = (fileID == 0)
                               ? static_cast<int32_t>(pathID)
                               : 0;
        }
    }
}