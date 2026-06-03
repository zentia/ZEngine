#include "Runtime/BaseClasses/ImmediatePtr.h"

#include "Runtime/BaseClasses/IPPtrResolver.h"
#include "Runtime/BaseClasses/LocalSerializedObjectIdentifier.h"
#include "Runtime/BaseClasses/Object.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/Core/Base/Macro.h"

void ImmediatePtrResolveWrite(const Object* ptr, int32_t& outFileID, int64_t& outPathID)
{
    outFileID = 0;
    outPathID = 0;
    if (ptr == nullptr)
        return;

    const int32_t instanceID = ptr->GetInstanceID();

    LocalSerializedObjectIdentifier lsoi;
    lsoi.localSerializedFileIndex = 0;
    lsoi.localIdentifierInFile = 0;

    IPPtrResolver* resolver = GetCurrentPPtrResolver();
    if (resolver != nullptr)
    {
        resolver->InstanceIDToLSOI(instanceID, lsoi);
    }
    else
    {
        // No resolver active (in-memory clipboard / undo style round-trips):
        // raw-stash the InstanceID into pathID so a same-process round-trip
        // resolves back to the identical object. Matches PPtr's fallback.
        lsoi.localSerializedFileIndex = 0;
        lsoi.localIdentifierInFile = static_cast<int64_t>(instanceID);
    }

    outFileID = lsoi.localSerializedFileIndex;
    outPathID = lsoi.localIdentifierInFile;
}

Object* ImmediatePtrResolveRead(int32_t fileID, int64_t pathID)
{
    int32_t instanceID = 0;

    IPPtrResolver* resolver = GetCurrentPPtrResolver();
    if (resolver != nullptr)
    {
        LocalSerializedObjectIdentifier lsoi;
        lsoi.localSerializedFileIndex = fileID;
        lsoi.localIdentifierInFile = pathID;
        instanceID = resolver->LSOIToInstanceID(lsoi);
    }
    else
    {
        instanceID = (fileID == 0) ? static_cast<int32_t>(pathID) : 0;
    }

    if (instanceID == 0)
        return nullptr;

    // ImmediatePtr targets live in the same file and are already produced /
    // registered by the time the referencing object is read, so the live
    // ObjectManager table is the source of truth (no lazy disk load needed,
    // unlike PPtr's cross-asset path).
    return GET_SYSTEM(ObjectManager)->IDToPointer(instanceID);
}
