#pragma once

// SerializedFilePPtrResolver -- IPPtrResolver implementation that
// ties PPtr round-trip into a SerializedFile's externals table for
// writes, and into an injectable lookup callback for reads. Designed
// to be lightweight and side-effect-isolated: it owns no engine
// state, and a fresh instance is constructed per read/write session
// so concurrent loads on different threads never share resolver
// state.
//
// PR-SE3a-refine deliberately keeps cross-asset reference resolution
// behind callback hooks rather than reaching into AssetManager
// directly. That keeps the touch surface small (no res_type changes
// in this PR) and matches the project rule that PR-SE3a-refine may
// not modify Resource/res_type/** or Function/**. The migrate sub-PR
// is what wires the AssetManager-side mapping in.

#include "Runtime/BaseClasses/IPPtrResolver.h"

#include <functional>
#include <stdint.h>
#include <string>
#include <unordered_map>

class SerializedFile;
struct FileIdentifier;

class SerializedFilePPtrResolver : public IPPtrResolver
{
public:
    // Optional injection points. Both default to "no-op" -- when not
    // set, only self-references resolve correctly and external refs
    // serialize as null on write / null on read. That's exactly the
    // behaviour exercised by P1-P3 of PptrSmokeTest.cpp.
    //
    // InstanceIDToFileIdentifier -- writer side. Given a non-zero
    // InstanceID that does NOT belong to the local SerializedFile,
    // return the FileIdentifier (guid + type + path) that names the
    // owning external file, plus the PathID inside that file. Return
    // false to mark the reference as dangling (resolver will write
    // (0, 0) i.e. null).
    using InstanceIDToFileIdentifierFn = std::function<bool(int32_t instanceID, FileIdentifier& outRef, int64_t& outPathID)>;

    // FileIdentifierToInstanceID -- reader side. Given an external
    // FileIdentifier (recovered from the on-disk externals table) and
    // an in-file PathID, return the runtime InstanceID. Return 0 to
    // mark the reference dangling (resolver propagates 0 as a null
    // PPtr -- consumer code that dereferences will get nullptr via
    // the existing ObjectManager fallthrough).
    using FileIdentifierToInstanceIDFn = std::function<int32_t(const FileIdentifier& ref, int64_t pathID)>;

    SerializedFilePPtrResolver(SerializedFile* file,
                               InstanceIDToFileIdentifierFn writerHook = {},
                               FileIdentifierToInstanceIDFn readerHook = {});

    // IPPtrResolver
    int32_t LSOIToInstanceID(const LocalSerializedObjectIdentifier& lsoi) override;
    void InstanceIDToLSOI(int32_t instanceID, LocalSerializedObjectIdentifier& out) override;

    // Local-target helpers. The SerializedFile-side identity for the
    // currently-being-written/read object is opaque to PPtr<T>::
    // Transfer (it just carries an InstanceID); the host code
    // (AssetManager / SerializedFile) tells us which (instanceID,
    // pathID) tuples count as "local" so a self-pointer round-trips
    // as (FileID=0, PathID=local).
    void RegisterLocalObject(int32_t instanceID, int64_t pathID);

private:
    SerializedFile* m_File;

    InstanceIDToFileIdentifierFn m_WriterHook;
    FileIdentifierToInstanceIDFn m_ReaderHook;

    // instanceID -> in-file pathID for objects living in m_File. Built
    // up by RegisterLocalObject before write; populated on the read
    // side by the host code (one entry per object in
    // SerializedFile::ObjectMap). Lookup hits here mean "self-ref"
    // and write FileID=0.
    std::unordered_map<int32_t, int64_t> m_LocalInstanceToPath;

    // Reverse map for reader's self-ref: pathID -> instanceID. Built
    // up via RegisterLocalObject too.
    std::unordered_map<int64_t, int32_t> m_LocalPathToInstance;
};
