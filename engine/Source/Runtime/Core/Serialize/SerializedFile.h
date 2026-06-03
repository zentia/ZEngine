#pragma once

#include "Runtime/BaseClasses/Type.h"
#include "Runtime/Core/Serialize/SerializationMetaFlags.h"
#include "Runtime/Core/Thread/AtomicRefCounter.h"
#include "Runtime/Utility/MemoryUtility.h"
#include "Runtime/VirtualFileSystem/VirtualFileSystemTypes.h"
#include "SerializationCaching/BufferedCacheWriter.h"
#include "SerializedFileLoadError.h"
#include "TypeTree.h"

#include <string>
#include <vector>

class Object;
class CacheReaderBase;
class CachedWriter;

struct FileIdentifier
{
    enum InsertMode
    {
        kDontCreate = 0,
        kCreate = 1,
        kAllowRemap = 2
    };

    // Unity-1:1 (SerializedFile.cpp:1675-1711, 1814-1825). Each entry
    // names one external SerializedFile this file's PPtrs may point
    // into. The triple is what gets persisted inside the metadata
    // buffer; resolvers map it to/from runtime asset paths or GUIDs.
    //
    //   guid     -- 32-char lowercase hex (matches AssetFileHeader.guid).
    //               Empty string permitted for in-flight legacy refs;
    //               in that case `pathName` is the source of truth.
    //   type     -- asset type-name hint (for incremental loading and
    //               schema audit). Optional, empty string is fine.
    //   pathName -- absolute or project-relative file path. Used as a
    //               fallback identifier when `guid` is empty and as a
    //               human-readable hint in the registry. Optional.
    //
    // We intentionally don't carry `assetType` as a numeric ID like
    // Unity does; ZEngine's stable type IDs aren't versioned across
    // git revisions yet, and a string is fine until they are. See
    // PR-SE3a-refine in AGENTS.md / PPTR_DESIGN.md §4.3.
    std::string guid;
    std::string type;
    std::string pathName;
};

class SerializedType
{
public:
    enum
    {
        kEqual = 0,
        kNotEqual = 1,
        kNotCompared = -1
    };
    using TypeVector = std::vector<SerializedType>;

    SerializedType(const Type* type);

    // Not copyable due to std::atomic member, but movable (copies atomic value)
    SerializedType(const SerializedType&) = delete;
    SerializedType& operator=(const SerializedType&) = delete;
    SerializedType(SerializedType&& other) noexcept;
    SerializedType& operator=(SerializedType&& other) noexcept;

    void CompareAgainstNewType(const Object& object, const TypeVector& refTypesPool, TransferInstructionFlags options) const;

    const TypeTree* GetOldType() const { return m_OldType; }
    void SetOldType(const TypeTree* t);

    void WriteType(std::vector<uint8_t>& cache);

    bool ReadType(const uint8_t*& iterator, const uint8_t* end);
    uint64_t GetPersistentTypeID() const { return m_Type == nullptr ? Type::UndefinedPersistentTypeID : m_Type->assetTypeID; }

private:
    const Type* m_Type;
    const TypeTree* m_OldType;
    mutable std::atomic<int> m_Equals;
    using TypeDependencies = std::vector<int32_t>;
    TypeDependencies m_TypeDependencies;
};

class SerializedFile
{
public:
    enum
    {
        kSectionAlignment = 16
    };
    ~SerializedFile();

    struct ObjectInfo
    {
        FileSize byteStart;
        uint32_t byteSize;
        uint64_t typeID;
    };
    using ObjectMap = std::unordered_map<int64_t, ObjectInfo>;
    SerializedFileLoadError InitializeWrite(CachedWriter& cachedWriter);
    SerializedFileLoadError InitializeRead(const std::filesystem::path& path, size_t cacheSize);

    void Release();

    void ReadObject(int64_t fileID, Object& object);
    void WriteObject(Object& object, int64_t fileID);
    uint64_t GetObjectPersistentTypeID(int64_t fileID) const;
    FileSize GetByteStart(int64_t id) const;
    uint32_t GetByteSize(int64_t id) const;

    bool FinishWriting(FileSize* outDataOffset);

    using TypeVector = SerializedType::TypeVector;

    struct ObjectData
    {
        int32_t persistentTypeID;
        std::vector<uint8_t> data;
        const TypeTree* typeTree;
    };
    bool ExtractObjectData(int64_t fileID, SerializedFile::ObjectData& data);

    // -------------------------------------------------------------------------
    // P2 #6 -- GUID persistence in `.zasset` header.
    //
    // Every `.zasset` produced by `WriteHeaderAndMetadata` is now prefixed
    // with a 176-byte `AssetFileHeader` (see `Runtime/asset/asset_file.h`)
    // carrying:
    //   * magic = "ZASS" (k_zasset_magic) so AssetRegistry can distinguish
    //     real binary .zasset from text/legacy ones.
    //   * `guid[37]`     -- the asset's stable GUID (32 hex + null terminator,
    //                       leaving 4 bytes spare).
    //   * `asset_type[64]` -- runtime type-name string for fast registry
    //                         classification without instantiating the asset.
    //   * `metadata_offset` / `metadata_size` / `data_offset` / `data_size`
    //     -- byte spans of the inner SerializedFile blob, relative to file
    //     start (so external readers can locate them without instantiating
    //     SerializedFile).
    //
    // After the AssetFileHeader prefix the existing SerializedFileHeader +
    // metadata + data streams are written unchanged. On read, ReadHeader()
    // peeks the magic and, if matched, advances `m_ReadOffset` past the
    // 176-byte prefix so all subsequent positional reads transparently skip
    // it. Files written before this PR (no ZASS prefix) are still readable
    // -- ReadHeader falls through with `m_ReadOffset = 0`.
    //
    // Setters below let WriteFile inject the GUID / asset-type just before
    // FinishWriting. Empty values fall back to a deterministic GUID derived
    // from the output path (so callers that don't care still get a stable,
    // reproducible identity, matching ScriptRegistry's path-hash policy).
    void SetAssetGuid(const std::string& guid) { m_AssetGuid = guid; }
    void SetAssetTypeName(const std::string& typeStr) { m_AssetTypeName = typeStr; }
    const std::string& GetAssetGuid() const { return m_AssetGuid; }
    const std::string& GetAssetTypeName() const { return m_AssetTypeName; }

    // -------------------------------------------------------------------------
    // PR-SE3a-refine -- ExternalReferences table.
    //
    // Each PPtr<T> serialized inside this file's data block carries a
    // (m_FileID, m_PathID) pair where m_FileID is an index into this
    // table:
    //   * 0          -> "self" (the target lives in this same file)
    //   * 1..N       -> external file at m_Externals[m_FileID - 1]
    //
    // The table is appended at the tail of the metadata buffer (i.e.
    // inside the bytes returned by BuildMetadataSection, after the
    // existing typeCount + types + objectCount + ObjectInfo entries).
    // This mirrors Unity's SerializedFile.cpp:1814-1825 layout 1:1 and
    // means the outer 176-byte AssetFileHeader prefix stays
    // binary-compatible -- AssetRegistry::ScanSingleAsset keeps reading
    // it back via direct ifstream::read with no changes. Format detail:
    // see PPTR_DESIGN.md §4.2-§4.4.
    //
    // AddExternalRef returns a 1-based index suitable for direct use as
    // an m_FileID. Calling it with an entry whose `guid` already exists
    // in the table dedups and returns the existing index. Index 0 is
    // never returned -- it's reserved for self-references and isn't
    // stored in this table.
    int32_t AddExternalRef(const FileIdentifier& ref);

    const std::vector<FileIdentifier>& GetExternalRefs() const { return m_Externals; }
    std::vector<FileIdentifier>& GetExternalRefs() { return m_Externals; }

private:
    void FinalizeInitCommon(TransferInstructionFlags options);
    SerializedFileLoadError FinalizeInitRead(TransferInstructionFlags options);
    SerializedFileLoadError FinalizeInitWrite(TransferInstructionFlags options);

    SerializedFileLoadError ReadHeader();

    void BuildMetadataSection(std::vector<uint8_t>& cache);
    void WriteHeaderAndMetadata(FileSize* outDataOffset = nullptr);

    bool ReadMetadata(int64_t version, FileSize dataOffset, uint8_t* const data, size_t length, FileSize dataFileEnd);

    CacheReaderBase* m_ReadFile;
    FileSize m_ReadOffset;
    ObjectMap m_Object;

    TransferInstructionFlags m_Options;

    uint8_t m_FileEndianess;

    TypeVector m_Types;
    TypeVector m_RefTypes;

    AtomicRefCounter m_RefCount;

    CachedWriter* m_CachedWriter;
    BufferedCacheWriter m_MemCacheWriter;

    // P2 #6 -- asset-identity fields stamped into the AssetFileHeader prefix
    // by WriteHeaderAndMetadata. Empty strings mean "synthesise from path"
    // (see SerializedFile.cpp). Read by ReadHeader if a ZASS prefix is found
    // so callers can query the on-disk identity post-load.
    std::string m_AssetGuid;
    std::string m_AssetTypeName;

    // PR-SE3a-refine -- ExternalReferences table. Populated on write by
    // SerializedFilePPtrResolver via AddExternalRef as PPtrs to other
    // files are observed; populated on read by ReadMetadata when the
    // inner SerializedFileHeader.version >= 1. Empty for legacy files
    // (version == 0) so PPtrs in those files always resolve as
    // self-references or null -- which is exactly what they did before
    // this PR (they didn't serialize at all).
    std::vector<FileIdentifier> m_Externals;
};

template<typename T>
void ReadHeaderCache(T& t, uint8_t const*& c)
{
    t = LoadUnaligned<T>(c);
    c += sizeof(T);
}

template<typename T>
void WriteHeaderCache(const T& t, std::vector<uint8_t>& vec)
{
    vec.resize(vec.size() + sizeof(T));
    T& dst = *reinterpret_cast<T*>(&vec[vec.size() - sizeof(T)]);
    dst = t;
}