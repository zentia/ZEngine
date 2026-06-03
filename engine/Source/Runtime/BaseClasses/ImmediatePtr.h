#pragma once

#include "Runtime/BaseClasses/LocalSerializedObjectIdentifier.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"

#include <stdint.h>

class Object;

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
void ImmediatePtrResolveWrite(const Object* ptr, int32_t& outFileID, int64_t& outPathID);
Object* ImmediatePtrResolveRead(int32_t fileID, int64_t pathID);

template<typename T>
class ImmediatePtr
{
public:
    ImmediatePtr()
        : m_Ptr(nullptr) {}
    ImmediatePtr(T* ptr)
        : m_Ptr(ptr) {}

    static const char* GetTypeString();

    static bool AllowTransferOptimization() { return false; }

    template<typename TransferFunction>
    void Transfer(TransferFunction& transfer);
    ImmediatePtr& operator=(T* ptr)
    {
        m_Ptr = ptr;
        return *this;
    }
    operator T*() const { return m_Ptr; }
    T* operator->() const
    {
        T* o = m_Ptr;
        return o;
    }

private:
    T* m_Ptr;
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
        m_Ptr = static_cast<T*>(ImmediatePtrResolveRead(fileID, pathID));
    }
}