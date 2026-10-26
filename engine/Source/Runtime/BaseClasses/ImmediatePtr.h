#pragma once

#include "Runtime/BaseClasses/LocalSerializedObjectIdentifier.h"
#include "Runtime/Core/Memory/UafReporter.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"

#include <stdint.h>

class Object;

// ZENGINE_LIFETIME_CHECK 由 CMake 注入（Debug/DebugV8/RelWithDebInfo=1，
// Release/MinSizeRel=0）。这里给一个自包含的兜底：若某个 TU 未经 CMake 注入
// （例如脱离主工程树单独编译），默认关闭检测，保持零开销、不误报。
#ifndef ZENGINE_LIFETIME_CHECK
    #define ZENGINE_LIFETIME_CHECK 0
#endif

// Resolver round-trip helpers for ImmediatePtr<T>::Transfer. Defined in
// ImmediatePtr.cpp so this header stays free of Object / ObjectManager /
// IPPtrResolver includes (it is pulled in extremely widely via Component.h).
// They take/return the type-erased Object* base; the templated Transfer below
// up/down-casts to T* at the (always-complete-type) instantiation site.
//
// Write: maps the target object's runtime InstanceID into an on-disk
//   (fileID, pathID) pair via the active per-thread IPPtrResolver (the same
//   one PPtr<T> uses). fileID==0 means "local target in this same file".
// Read: maps a deserialized (fileID, pathID) back to the live Object* via the
//   resolver + ObjectManager. Returns nullptr for null / unresolved refs.
//
// out_instanceID (optional): 若调用方需要，回填解析出的 InstanceID，供
//   ImmediatePtr 缓存下来做运行期 UAF 校验。无 resolver 的 fallback 路径同样能
//   拿回 InstanceID（它本就是把 InstanceID 塞进 pathID 的）。传 nullptr 表示不关心。
void ImmediatePtrResolveWrite(const Object* ptr, int32_t& outFileID, int64_t& outPathID);
Object* ImmediatePtrResolveRead(int32_t fileID, int64_t pathID, int32_t* out_instanceID = nullptr);

template<typename T>
class ImmediatePtr
{
public:
    ImmediatePtr()
        : m_Ptr(nullptr), m_InstanceID(0) {}
    ImmediatePtr(T* ptr)
        : m_Ptr(ptr), m_InstanceID(0) {}

    static const char* GetTypeString();

    static bool AllowTransferOptimization() { return false; }

    template<typename TransferFunction>
    void Transfer(TransferFunction& transfer);
    ImmediatePtr& operator=(T* ptr)
    {
        m_Ptr = ptr;
        // 裸指针赋值没有 instanceID 来源，置 0 退化为"不校验"。
        // 不强行反查 ObjectManager（那会把这个 header-only 类型拖进 Object 依赖，
        // 且赋值 RHS 未必是 Object）。此类 ImmediatePtr 不参与 UAF 检测，
        // 与改造前的裸指针语义一致。
        m_InstanceID = 0;
        return *this;
    }
    operator T*() const { return ResolveWithCheck("ImmediatePtr<T>::operator T*"); }
    T* operator->() const { return ResolveWithCheck("ImmediatePtr<T>::operator->"); }

private:
    // 解引用前的悬垂校验。集中一处，operator T*() 与 operator->() 复用。
    //
    // 判据：指针非空、有 instanceID、但该 id 已从 ObjectManager 表注销
    // → 对象已销毁仍被访问 = use-after-free。零身份（instanceID==0）的对象
    // 不参与，避免对裸赋值/未登记对象误报。Release 下 ZENGINE_LIFETIME_CHECK=0，
    // 直接返回裸指针，与改造前完全一致（零开销）。
    T* ResolveWithCheck(const char* access_site) const
    {
#if ZENGINE_LIFETIME_CHECK
        if (m_Ptr != nullptr && m_InstanceID != 0 &&
            !zengine::lifetime::IsInstanceAlive(m_InstanceID))
        {
            zengine::lifetime::ReportUseAfterFree(m_InstanceID, access_site);
        }
#endif
        return m_Ptr;
    }

    T* m_Ptr;
    // 目标对象的 InstanceID，由 Transfer 的反序列化路径回填（见 ImmediatePtr.cpp
    // 的 ImmediatePtrResolveRead）。运行时缓存，**不参与序列化**——wire format
    // 仍是 (fileID, pathID)，因此本改动不改变任何 .zasset 的序列化布局。
    // instanceID==0 表示"无身份"（裸赋值 / 尚未解析），不触发 UAF 检测。
    int32_t m_InstanceID;
};

template<typename T>
inline const char* ImmediatePtr<T>::GetTypeString()
{
    return T::GetPPtrTypeString();
}

template<typename T>
template<typename TransferFunction>
inline void ImmediatePtr<T>::Transfer(TransferFunction& transfer)
{
    // Wire format is the Unity (m_FileID:int32, m_PathID:int64) pair, identical
    // to PPtr<T> -- the only difference is ImmediatePtr targets are expected to
    // live in the SAME file (fileID 0), so cross-file resolution is not the
    // common path. The two transfer.Transfer() calls are direction-agnostic and
    // produce a stable {m_FileID, m_PathID} TypeTree under GenerateTypeTreeTransfer.
    int32_t fileID = 0;
    int64_t pathID = 0;

    if constexpr (TransferFunction::IsWriting())
    {
        ImmediatePtrResolveWrite(m_Ptr, fileID, pathID);
    }

    transfer.Transfer(fileID, "m_FileID", TransferMetaFlags::HideInEditorMask);
    transfer.Transfer(pathID, "m_PathID", TransferMetaFlags::HideInEditorMask);

    if constexpr (TransferFunction::IsReading())
    {
        int32_t resolved_id = 0;
        m_Ptr = static_cast<T*>(ImmediatePtrResolveRead(fileID, pathID, &resolved_id));
        // 缓存 InstanceID 供运行期 UAF 校验。这是运行时镜像，不写回 wire format
        // （上面只 Transfer 了 fileID/pathID），因此序列化布局不变。
        m_InstanceID = resolved_id;
    }
}