#pragma once

#include "IPPtrResolver.h"
#include "LocalSerializedObjectIdentifier.h"
#include "Object.h"
#include "ObjectManager.h"

#include <stdint.h>

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
        temp = ReadObjectFromPersistentManager(m_InstanceID);
    return static_cast<T*>(temp);
}

template<typename T>
inline T* PPtr<T>::operator->() const
{
    Object* temp = GET_SYSTEM(ObjectManager)->IDToPointer(m_InstanceID);
    return static_cast<T*>(temp);
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