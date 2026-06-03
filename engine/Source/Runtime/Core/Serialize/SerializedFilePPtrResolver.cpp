#include "SerializedFilePPtrResolver.h"

#include "SerializedFile.h"

SerializedFilePPtrResolver::SerializedFilePPtrResolver(SerializedFile* file,
                                                       InstanceIDToFileIdentifierFn writerHook,
                                                       FileIdentifierToInstanceIDFn readerHook)
    : m_File(file), m_WriterHook(std::move(writerHook)), m_ReaderHook(std::move(readerHook))
{
}

void SerializedFilePPtrResolver::RegisterLocalObject(int32_t instanceID, int64_t pathID)
{
    if (instanceID == 0 || pathID == 0)
        return;

    m_LocalInstanceToPath[instanceID] = pathID;
    m_LocalPathToInstance[pathID] = instanceID;
}

int32_t SerializedFilePPtrResolver::LSOIToInstanceID(const LocalSerializedObjectIdentifier& lsoi)
{
    // Null pointer encoded as (0, 0). Normalises to InstanceID 0,
    // matching the empty-PPtr default.
    if (lsoi.localSerializedFileIndex == 0 && lsoi.localIdentifierInFile == 0)
        return 0;

    if (lsoi.localSerializedFileIndex == 0)
    {
        // Self-reference. Look up the path in the local registration
        // map; missing entries mean the target object isn't in the
        // current load session -- treat as null. Unity does the same
        // (ILSOIResolver::LSOIToInstanceID returns 0 for unknown
        // local ids; consumer code falls through to ObjectManager).
        auto it = m_LocalPathToInstance.find(lsoi.localIdentifierInFile);
        return (it != m_LocalPathToInstance.end()) ? it->second : 0;
    }

    // External reference: m_FileID is 1-based index into m_Externals.
    if (m_File == nullptr || !m_ReaderHook)
        return 0;

    const auto& externals = m_File->GetExternalRefs();
    int32_t idx = lsoi.localSerializedFileIndex - 1;
    if (idx < 0 || static_cast<size_t>(idx) >= externals.size())
        return 0;

    return m_ReaderHook(externals[idx], lsoi.localIdentifierInFile);
}

void SerializedFilePPtrResolver::InstanceIDToLSOI(int32_t instanceID, LocalSerializedObjectIdentifier& out)
{
    out.localSerializedFileIndex = 0;
    out.localIdentifierInFile = 0;

    if (instanceID == 0)
        return;

    // Self first: cheap unordered_map lookup, no callback hop.
    auto localIt = m_LocalInstanceToPath.find(instanceID);
    if (localIt != m_LocalInstanceToPath.end())
    {
        out.localSerializedFileIndex = 0;
        out.localIdentifierInFile = localIt->second;
        return;
    }

    // External: ask the host hook for (FileIdentifier, pathID), then
    // dedup-insert the FileIdentifier into the file's externals
    // table. AddExternalRef returns the 1-based index used as
    // m_FileID. Missing hook => leave (0, 0); the on-disk PPtr will
    // be a null reference, which is what users get today anyway since
    // no res_type currently serialises PPtrs.
    if (m_File == nullptr || !m_WriterHook)
        return;

    FileIdentifier ref;
    int64_t pathID = 0;
    if (!m_WriterHook(instanceID, ref, pathID))
        return;

    int32_t fileIndex = m_File->AddExternalRef(ref);
    out.localSerializedFileIndex = fileIndex;
    out.localIdentifierInFile = pathID;
}
