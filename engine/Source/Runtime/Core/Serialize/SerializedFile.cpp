#include "SerializedFile.h"

#include "Runtime/Asset/AssetFile.h"  // P2 #6 -- AssetFileHeader / k_zasset_magic
#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Serialize/SerializationCaching/CachedReader.h"
#include "Runtime/Core/Serialize/SerializationCaching/FileCacherRead.h"
#include "Runtime/Core/Serialize/SerializationCaching/MemoryCacheReader.h"
#include "Runtime/Core/Serialize/TransferFunctions/SafeBinaryRead.h"
#include "Runtime/Core/Serialize/TransferFunctions/StreamedBinaryRead.h"
#include "Runtime/Core/Serialize/TypeTreeCache.h"
#include "Runtime/Utility/Align.h"
#include "Runtime/Utility/Utility.h"
#include "SerializationCaching/MemoryCacherReadBlocks.h"
#include "SwapEndianBytes.h"
#include "TypeTreeQueries.h"

#include <array>
#include <atomic>
#include <cctype>
#include <cstring>

static int32_t FindOrCreateSerializedTypeForType(SerializedType::TypeVector& serializedTypes, const Type* type)
{
    uint64_t findPersistentTypeID = (type == nullptr) ? Type::UndefinedPersistentTypeID : type->assetTypeID;
    for (int i = 0; i < serializedTypes.size(); ++i)
    {
        const SerializedType& serializedType = serializedTypes[i];
        uint64_t serializedPersistentTypeID = serializedType.GetPersistentTypeID();
        if (serializedPersistentTypeID == findPersistentTypeID)
        {
            return i;
        }
    }

    serializedTypes.emplace_back(type);

    return serializedTypes.size() - 1;
}

SerializedFile::~SerializedFile()
{
    MEMORY_DELETE(m_ReadFile);
}

// PR-SE3a-refine -- add or dedup an external file reference. Returns
// the 1-based index suitable for direct use as PPtr::m_FileID. Index 0
// is reserved for self-references and intentionally never returned.
//
// Dedup key is the GUID when non-empty, otherwise the path. This lets
// callers feed in a "guid known but path unknown" entry once and not
// duplicate it when the same target is observed again later in the
// same write session via a different code path.
int32_t SerializedFile::AddExternalRef(const FileIdentifier& ref)
{
    const bool hasGuid = !ref.guid.empty();
    const std::string& key = hasGuid ? ref.guid : ref.pathName;

    for (size_t i = 0; i < m_Externals.size(); ++i)
    {
        const FileIdentifier& existing = m_Externals[i];
        const std::string& existingKey =
            !existing.guid.empty() ? existing.guid : existing.pathName;

        if (!key.empty() && existingKey == key)
            return static_cast<int32_t>(i + 1);
    }

    m_Externals.push_back(ref);
    return static_cast<int32_t>(m_Externals.size());
}

void SerializedType::SetOldType(const TypeTree* t)
{
    m_OldType = t;
}

SerializedType::SerializedType(const Type* type)
    : m_Type(type), m_OldType(nullptr), m_Equals(kNotCompared) {}

SerializedType::SerializedType(SerializedType&& other) noexcept
    : m_Type(other.m_Type), m_OldType(other.m_OldType), m_Equals(other.m_Equals.load()), m_TypeDependencies(std::move(other.m_TypeDependencies))
{
    other.m_Type = nullptr;
    other.m_OldType = nullptr;
}

SerializedType& SerializedType::operator=(SerializedType&& other) noexcept
{
    if (this != &other)
    {
        m_Type = other.m_Type;
        m_OldType = other.m_OldType;
        m_Equals.store(other.m_Equals.load());
        m_TypeDependencies = std::move(other.m_TypeDependencies);
        other.m_Type = nullptr;
        other.m_OldType = nullptr;
    }
    return *this;
}

void SerializedType::CompareAgainstNewType(const Object& object, const TypeVector& refTypesPool, TransferInstructionFlags options) const
{
    int newEquals = kNotEqual;

    TypeTree newerType;
    GET_SYSTEM(TypeTreeCache)->GetTypeTree(&object, options, newerType);
    if (m_OldType != nullptr && TypeTreeQueries::IsStreamedBinaryCompatible(m_OldType->Root(), newerType.Root()))
    {
        int32_t dependenciesCount = m_TypeDependencies.size();
    }
    m_Equals.exchange(newEquals);
}

void SerializedType::WriteType(std::vector<uint8_t>& cache)
{
    TypeTree::WriteTypeTree(*m_OldType, cache);
}

bool SerializedType::ReadType(const uint8_t*& iterator, const uint8_t* end)
{
    TypeTree* typeTree = MemoryManager::CreateObject<TypeTree>();
    if (!TypeTree::ReadTypeTree(*typeTree, iterator, end))
    {
        MemoryManager::DestroyObject(typeTree);
        return false;
    }
    m_OldType = typeTree;
    return true;
}

SerializedFileLoadError SerializedFile::InitializeWrite(CachedWriter& cachedWriter)
{
    m_CachedWriter = &cachedWriter;
    m_CachedWriter->PushWriter(m_MemCacheWriter);
    return FinalizeInitWrite(kNoTransferInstructionFlags);
}

SerializedFileLoadError SerializedFile::InitializeRead(const std::filesystem::path& path, size_t cacheSize)
{
    m_ReadFile = MemoryManager::CreateObject<FileCacherRead>(path, cacheSize);
    return FinalizeInitRead(kNoTransferInstructionFlags);
}

void SerializedFile::Release()
{
    if (m_RefCount.Release())
    {
        MemoryManager::DestroyObject(this);
    }
}

void SerializedFile::ReadObject(int64_t fileID, Object& object)
{
    ObjectMap::const_iterator iter = m_Object.find(fileID);
    if (iter == m_Object.end())
        return;

    const ObjectInfo& info = iter->second;

    const SerializedType& serializedType = m_Types[info.typeID];
    serializedType.CompareAgainstNewType(object, m_RefTypes, m_Options);

    FileSize byteStart = info.byteStart + m_ReadOffset;

    // -------------------------------------------------------------------------
    // Schema-aware read path (SafeBinaryRead, see doc/asset_management/
    // SCHEMA_EVOLUTION_AND_TYPETREE.md).
    //
    // Every `.zasset` written via SerializedFile::WriteObject already carries
    // a per-type TypeTree blob inside the metadata section (populated below in
    // WriteObject -> SetOldType, written out by SerializedType::WriteType).
    // ReadHeader recovers it into SerializedType::m_OldType. With both ends
    // wired, we can drive the read through SafeBinaryRead, which looks each
    // field up by name in the on-disk TypeTree -- making missing fields
    // (deleted / renamed since the file was written) silently skipped, and
    // newly-added fields default-initialised. That is the point of this whole
    // exercise: Object subclasses can evolve their Transfer() schema without
    // breaking older .zasset files.
    //
    // Fallback: if m_OldType is null (e.g. a pre-PR-SE1 file written before
    // the metadata section was populated, or any future writer path that
    // forgets to call SetOldType), we silently degrade to the legacy
    // StreamedBinaryRead path. This keeps every existing on-disk byte
    // readable -- new readers vs old files is a one-way compatibility we
    // intentionally support.
    const TypeTree* oldType = serializedType.GetOldType();
    if (oldType != nullptr)
    {
        SafeBinaryRead readStream;
        CachedReader& cache = readStream.Init(oldType->Root(), byteStart, info.byteSize, kReadWriteFromSerializedFile);
        cache.InitRead(*m_ReadFile, byteStart.Cast<size_t>(), info.byteSize);
        object.VirtualRedirectTransfer(readStream);
        cache.End();
    }
    else
    {
        LOG_INFO(ZSerializer, "ReadObject: no TypeTree available for fileID={} (legacy / pre-typetree .zasset?), falling back to StreamedBinaryRead", fileID);
        StreamedBinaryRead readStream;
        CachedReader& cache = readStream.Init(kReadWriteFromSerializedFile);
        cache.InitRead(*m_ReadFile, byteStart.Cast<size_t>(), info.byteSize);
        object.VirtualRedirectTransfer(readStream);
    }
}

void SerializedFile::WriteObject(Object& object, int64_t fileID)
{
    int32_t typeID = FindOrCreateSerializedTypeForType(m_Types, object.GetType());
    SerializedType& serializedType = m_Types[typeID];
    if (serializedType.GetOldType() == nullptr)
    {
        TypeTree* typeTree = MemoryManager::CreateObject<TypeTree>();
        GET_SYSTEM(TypeTreeCache)->GetTypeTree(&object, kDontRequireAllMetaFlags, *typeTree);
        serializedType.SetOldType(typeTree);
    }
    StreamedBinaryWrite writeStream;

    const size_t kFileAlignment = 8;

    uint64_t unalignedByteStart = m_CachedWriter->GetPosition().Cast<uint64_t>();

    uint64_t alignedByteStart = unalignedByteStart;

    if (unalignedByteStart % kFileAlignment != 0)
        alignedByteStart += kFileAlignment - unalignedByteStart % kFileAlignment;

    ObjectInfo& info = m_Object[fileID];
    info.byteStart = alignedByteStart;
    info.typeID = static_cast<uint64_t>(typeID);

    CachedWriter& cache = writeStream.Init(*m_CachedWriter);
    object.VirtualRedirectTransfer(writeStream);
    *m_CachedWriter = cache;

    info.byteSize = (m_CachedWriter->GetPosition() - info.byteStart).Cast<uint32_t>();
}

uint64_t SerializedFile::GetObjectPersistentTypeID(int64_t fileID) const
{
    ObjectMap::const_iterator object_iter = m_Object.find(fileID);
    if (object_iter == m_Object.end())
    {
        return Type::UndefinedPersistentTypeID;
    }

    const uint64_t type_index = object_iter->second.typeID;
    if (type_index >= m_Types.size())
    {
        return Type::UndefinedPersistentTypeID;
    }

    return m_Types[static_cast<size_t>(type_index)].GetPersistentTypeID();
}

FileSize SerializedFile::GetByteStart(int64_t id) const
{
    ObjectMap::const_iterator i = m_Object.find(id);
    return i->second.byteStart;
}

uint32_t SerializedFile::GetByteSize(int64_t id) const
{
    auto i = m_Object.find(id);
    return i->second.byteSize;
}

bool SerializedFile::FinishWriting(FileSize* outDataOffset)
{
    if (m_CachedWriter != nullptr)
    {
        m_CachedWriter->PopWriter();

        WriteHeaderAndMetadata(outDataOffset);

        m_MemCacheWriter.WriteAndClose(*m_CachedWriter);

        return m_CachedWriter->CompleteWriting();
    }
    return false;
}

void SerializedFile::FinalizeInitCommon(TransferInstructionFlags options)
{
}

SerializedFileLoadError SerializedFile::FinalizeInitRead(TransferInstructionFlags options)
{
    return ReadHeader();
}

SerializedFileLoadError SerializedFile::FinalizeInitWrite(TransferInstructionFlags options)
{
    FinalizeInitCommon(options);
    return kSerializedFileLoadError_None;
}

struct SerializedFileHeader
{
    int64_t version;
    FileSize metadataSize;
    FileSize fileSize;
    FileSize dataOffset;
    uint8_t endianess;

    void SwapEndianess()
    {
        uint64_t x;
        x = metadataSize.Cast<uint64_t>();
        SwapEndianBytes(x);
        metadataSize = x;

        x = fileSize.Cast<uint64_t>();
        SwapEndianBytes(x);
        fileSize = x;

        x = dataOffset.Cast<uint64_t>();
        SwapEndianBytes(x);
        dataOffset = x;

        SwapEndianBytes(*(uint32_t*)&version);
    }
};

template<typename T>
inline static bool VerifyCanRead(const T& val, const uint8_t* iterator, const uint8_t* end)
{
    return (iterator + sizeof(val)) <= end;
}

template<typename T>
inline static bool ReadHeaderCacheChecked(T& t, uint8_t const*& iterator, uint8_t const*& end)
{
    if (VerifyCanRead(t, iterator, end))
    {
        ReadHeaderCache(t, iterator);
        return true;
    }
    return false;
}

static void Write4Alignment(std::vector<uint8_t>& cache)
{
    uint32_t leftOver = Align4LeftOver(cache.size());
    uint8_t value = 0;
    for (uint32_t i = 0; i < leftOver; ++i)
    {
        WriteHeaderCache(value, cache);
    }
}

static void Read4Alignment(uint8_t const* data, uint8_t const*& iterator, uint8_t const* end)
{
    uint32_t offset = iterator - data;
    offset = ((offset + 3) >> 2) << 2;

    iterator = data + offset;
}

// PR-SE3a-refine -- length-prefixed string helpers for the externals
// table inside the metadata buffer. Format:
//   uint32 length, length bytes of UTF-8 (no null terminator).
// `length` is bounds-checked against the remaining buffer to avoid
// runaway reads on a corrupted file -- matches the defensive style of
// the existing typeCount / objectCount checks.
static void WriteLengthPrefixedString(const std::string& s, std::vector<uint8_t>& cache)
{
    uint32_t length = static_cast<uint32_t>(s.size());
    WriteHeaderCache(length, cache);
    if (length != 0)
    {
        const size_t offset = cache.size();
        cache.resize(offset + length);
        std::memcpy(cache.data() + offset, s.data(), length);
    }
}

static bool ReadLengthPrefixedString(std::string& s, uint8_t const*& iterator, uint8_t const* end)
{
    uint32_t length = 0;
    if (!ReadHeaderCacheChecked(length, iterator, end))
        return false;

    // Sanity bound: refuse anything larger than the remaining buffer.
    // Strings here are file paths or asset-type names -- a few kB at
    // worst -- so a corrupted multi-MB length is the canonical "this
    // file isn't what we think it is" signal.
    if (static_cast<size_t>(end - iterator) < length)
        return false;

    if (length == 0)
    {
        s.clear();
    }
    else
    {
        s.assign(reinterpret_cast<const char*>(iterator), length);
        iterator += length;
    }
    return true;
}

template<typename T>
static T ReadLocalIdentifier(int64_t version, uint8_t const* data, uint8_t const*& iterator, uint8_t const* end)
{
    int64_t fileID64 = 0;
    Read4Alignment(data, iterator, end);
    ReadHeaderCacheChecked(fileID64, iterator, end);
    return static_cast<T>(fileID64);
}

SerializedFileLoadError SerializedFile::ReadHeader()
{
    if (m_ReadFile == nullptr)
    {
        return kSerializedFileLoadError_InvalidHeader;
    }

    const size_t file_length = m_ReadFile->GetFileLength();
    if (file_length < sizeof(SerializedFileHeader))
    {
        return kSerializedFileLoadError_InvalidHeader;
    }

    // -------------------------------------------------------------------------
    // P2 #6 -- ZASS prefix detection.
    //
    // Every `.zasset` produced by `WriteHeaderAndMetadata` after this PR has a
    // 176-byte `AssetFileHeader` stamped at offset 0, with magic = "ZASS"
    // (k_zasset_magic). On read we peek the first 4 bytes; if they match we:
    //   1. Read the full 176-byte prefix and stash GUID + asset_type so
    //      callers (and AssetRegistry, indirectly) can query identity
    //      post-load via GetAssetGuid() / GetAssetTypeName().
    //   2. Set m_ReadOffset = sizeof(AssetFileHeader) = 176. All subsequent
    //      positional reads in this function (and in ReadObject /
    //      ExtractObjectData) already add m_ReadOffset to their byte
    //      positions, so the inner SerializedFileHeader/metadata/data layout
    //      stays unchanged -- we just transparently skip the prefix.
    //
    // If the magic doesn't match (legacy files written before this PR, or
    // non-zasset SerializedFile users), we fall through with m_ReadOffset = 0
    // and read the SerializedFileHeader directly from offset 0 as before.
    // This is what keeps existing on-disk caches (player scenes baked before
    // this change, importer scratch files, etc.) loadable.
    if (file_length >= sizeof(AssetFileHeader))
    {
        std::array<uint8_t, 4> magic_bytes {};
        ReadFileCache(*m_ReadFile, magic_bytes.data(), m_ReadOffset, magic_bytes.size());
        uint32_t magic_le = 0;
        std::memcpy(&magic_le, magic_bytes.data(), sizeof(magic_le));
        if (magic_le == k_zasset_magic)
        {
            std::array<uint8_t, sizeof(AssetFileHeader)> raw_prefix {};
            ReadFileCache(*m_ReadFile, raw_prefix.data(), m_ReadOffset, raw_prefix.size());
            AssetFileHeader prefix {};
            std::memcpy(&prefix, raw_prefix.data(), sizeof(prefix));

            // Bound the C-string construction defensively in case a writer
            // ever forgets to null-terminate the last byte (the canonical
            // writer below zero-initialises the prefix struct, but mirroring
            // asset_registry.cpp's defensive read keeps the two paths
            // symmetric).
            m_AssetGuid.assign(prefix.guid,
                               ::strnlen(prefix.guid, sizeof(prefix.guid)));
            m_AssetTypeName.assign(prefix.asset_type,
                                   ::strnlen(prefix.asset_type, sizeof(prefix.asset_type)));

            m_ReadOffset = static_cast<uint64_t>(sizeof(AssetFileHeader));
        }
    }

    std::array<uint8_t, sizeof(SerializedFileHeader)> raw_header {};
    ReadFileCache(*m_ReadFile, raw_header.data(), m_ReadOffset, raw_header.size());

    for (uint8_t byte : raw_header)
    {
        if (!std::isspace(byte))
        {
            if (byte == '{' || byte == '[')
            {
                return kSerializedFileLoadError_InvalidHeader;
            }
            break;
        }
    }

    SerializedFileHeader header {};
    std::memcpy(&header, raw_header.data(), sizeof(header));
    header.SwapEndianess();

    const uint64_t metadata_offset_value = sizeof(SerializedFileHeader);
    const uint64_t metadata_size_value = header.metadataSize.Cast<uint64_t>();
    const uint64_t file_size_value = header.fileSize.Cast<uint64_t>();
    const uint64_t data_offset_value = header.dataOffset.Cast<uint64_t>();
    // P2 #6 -- on a ZASS-prefixed file `actual_file_size` includes the
    // 176-byte prefix, but `header.fileSize` was computed by the writer
    // *excluding* the prefix (it's the size of the inner SerializedFile
    // blob only). Subtract m_ReadOffset so the bounds check works in both
    // legacy (offset == 0) and prefixed (offset == sizeof(AssetFileHeader))
    // modes without changing the on-disk encoding of fileSize.
    const uint64_t actual_file_size = static_cast<uint64_t>(file_length) - m_ReadOffset.Cast<uint64_t>();

    if (file_size_value == 0 || metadata_size_value == 0 || file_size_value > actual_file_size ||
        data_offset_value < metadata_offset_value || data_offset_value > file_size_value ||
        metadata_size_value > (data_offset_value - metadata_offset_value))

    {
        return kSerializedFileLoadError_InvalidHeader;
    }

    FileSize metadataSize, metadataOffset;
    FileSize dataSize, dataOffset;
    FileSize dataEnd;

    metadataOffset = FileSize(metadata_offset_value);
    metadataSize = header.metadataSize;
    m_FileEndianess = header.endianess;
    dataOffset = header.dataOffset;
    dataSize = FileSize(file_size_value - data_offset_value);

    std::vector<uint8_t> metadataBuffer;
    metadataBuffer.resize(static_cast<size_t>(metadata_size_value));
    if (metadata_size_value > 0)
    {
        ReadFileCache(*m_ReadFile, metadataBuffer.data(), m_ReadOffset + metadataOffset, static_cast<size_t>(metadata_size_value));
    }

    const bool metaDataRead = ReadMetadata(header.version, dataOffset, metadataBuffer.data(), metadataBuffer.size(), dataEnd);
    return metaDataRead ? kSerializedFileLoadError_None : kSerializedFileLoadError_InvalidMetadata;
}

void SerializedFile::BuildMetadataSection(std::vector<uint8_t>& cache)
{
    int32_t typeCount = m_Types.size();
    WriteHeaderCache(typeCount, cache);
    for (int i = 0; i < typeCount; ++i)
    {
        m_Types[i].WriteType(cache);
    }

    int32_t objectCount = m_Object.size();
    WriteHeaderCache(objectCount, cache);
    for (ObjectMap::iterator i = m_Object.begin(); i != m_Object.end(); i++)
    {
        Write4Alignment(cache);
        WriteHeaderCache(i->first, cache);

        WriteHeaderCache((i->second.byteStart).Cast<uint64_t>(), cache);
        WriteHeaderCache(i->second.byteSize, cache);
        WriteHeaderCache(i->second.typeID, cache);
    }

    // PR-SE3a-refine -- ExternalReferences table. Mirrors Unity's
    // SerializedFile.cpp:1814-1825: write the count up front, then
    // each entry's guid + type + (has_path, path). Entry count of 0
    // is fine and emitted unconditionally so the on-disk layout for
    // version-1 files is fixed-shape (parser doesn't have to check
    // whether the table is present). Aligned to 4 first to keep the
    // tail consistent with the per-object alignment above -- the
    // reader doesn't strictly need it but it costs at most 3 bytes
    // and keeps `xxd` output readable.
    Write4Alignment(cache);
    int32_t externalsCount = static_cast<int32_t>(m_Externals.size());
    WriteHeaderCache(externalsCount, cache);
    for (const FileIdentifier& ref : m_Externals)
    {
        WriteLengthPrefixedString(ref.guid, cache);
        WriteLengthPrefixedString(ref.type, cache);
        uint8_t hasPath = ref.pathName.empty() ? 0 : 1;
        WriteHeaderCache(hasPath, cache);
        if (hasPath)
            WriteLengthPrefixedString(ref.pathName, cache);
    }
}

void SerializedFile::WriteHeaderAndMetadata(FileSize* outDataOffset)
{
    // -------------------------------------------------------------------------
    // P2 #6 -- prepend a 176-byte AssetFileHeader prefix so AssetRegistry's
    // magic-number check actually succeeds on real `.zasset` files.
    //
    // Layout produced by this function (file offsets):
    //   [0  .. 176)   AssetFileHeader prefix (magic="ZASS", guid, asset_type,
    //                                          metadata_offset/size, data_*).
    //   [176 .. 176+sizeof(SerializedFileHeader))  SerializedFileHeader.
    //   [176+sizeof(SerializedFileHeader) .. 176 + realSize)  metadata buffer
    //                  (rounded up to kSectionAlignment).
    //   [176+realSize .. 176+realSize+m_MemCacheWriter.Size())  object data.
    //
    // The SerializedFileHeader's stored `fileSize` and `dataOffset` are
    // expressed RELATIVE to the start of the SerializedFileHeader (i.e. they
    // EXCLUDE the prefix). This is intentional -- it keeps the inner format
    // identical to the legacy on-disk encoding so old loaders / cached blobs
    // that were written before this PR keep working when re-read by code
    // built after it. ReadHeader compensates by tracking `m_ReadOffset`.
    std::vector<uint8_t> metadataBuffer;

    metadataBuffer.resize(sizeof(SerializedFileHeader));

    BuildMetadataSection(metadataBuffer);

    int metadataSize = metadataBuffer.size() - sizeof(SerializedFileHeader);
    int realSize = RoundUp(metadataBuffer.size(), kSectionAlignment);

    if (metadataBuffer.size() < realSize)
        metadataBuffer.resize(realSize);

    SerializedFileHeader& header = *(SerializedFileHeader*)metadataBuffer.data();

    // PR-SE3a-refine -- bump inner-header version 0 -> 1. Files
    // produced by this code path always include the externals table
    // appended by BuildMetadataSection (count may be 0). The 176-byte
    // outer ZASS prefix and its `version` field are NOT touched, so
    // AssetRegistry::ScanSingleAsset keeps reading the prefix
    // bit-for-bit identically to before this PR.
    header.version = 1;
    header.metadataSize = (uint64_t)metadataSize;
    header.fileSize = (uint64_t)(metadataBuffer.size() + m_MemCacheWriter.Size());
    header.dataOffset = (uint64_t)realSize;

    if (outDataOffset)
    {
        // outDataOffset is the absolute byte position where object data
        // starts in the on-disk file. Add the prefix so callers (player
        // loaders, asset preprocessors) get the right absolute offset.
        (*outDataOffset) = (uint64_t)(realSize + sizeof(AssetFileHeader));
    }

    header.SwapEndianess();

    // Build the 176-byte ZASS prefix. Zero-init guarantees:
    //   * `reserved[]` is fully zeroed (forward-compat for future fields).
    //   * `guid[]` / `asset_type[]` get an implicit null terminator beyond
    //     the bytes we copy in -- so strnlen-bounded reads on the registry
    //     side stay safe even if the input strings are exactly 36 / 63
    //     chars long.
    AssetFileHeader prefix {};
    prefix.magic = k_zasset_magic;
    prefix.version = k_zasset_version;

    // GUID is stored as 36 hex chars + null. Truncate longer inputs and
    // pad shorter ones (zero-init already filled the trailing bytes). The
    // upstream contract is that callers feed in either an empty string
    // (meaning "synthesise from path" -- handled in AssetManager::WriteFile)
    // or a valid 32-hex deterministic GUID string from
    // ScriptRegistry::deterministicGuidFromPath. Asserting here would just
    // crash player builds on malformed input, so we silently truncate.
    {
        const size_t copy_len = (m_AssetGuid.size() < sizeof(prefix.guid) - 1)
                                    ? m_AssetGuid.size()
                                    : (sizeof(prefix.guid) - 1);
        std::memcpy(prefix.guid, m_AssetGuid.data(), copy_len);
    }
    {
        const size_t copy_len = (m_AssetTypeName.size() < sizeof(prefix.asset_type) - 1)
                                    ? m_AssetTypeName.size()
                                    : (sizeof(prefix.asset_type) - 1);
        std::memcpy(prefix.asset_type, m_AssetTypeName.data(), copy_len);
    }

    prefix.metadata_offset = sizeof(AssetFileHeader);
    prefix.metadata_size = static_cast<uint64_t>(realSize);
    prefix.data_offset = sizeof(AssetFileHeader) + static_cast<uint64_t>(realSize);
    prefix.data_size = static_cast<uint64_t>(m_MemCacheWriter.Size());

    m_CachedWriter->Write(&prefix, sizeof(prefix));
    m_CachedWriter->Write(metadataBuffer.data(), metadataBuffer.size());
}

bool SerializedFile::ReadMetadata(int64_t version, FileSize dataOffset, uint8_t* const data, size_t length, FileSize dataFileEnd)
{
    uint8_t const *iterator = data, *end = data + length;

    int32_t typeCount = 0;
    if (!ReadHeaderCacheChecked(typeCount, iterator, end) || typeCount < 0 || static_cast<size_t>(typeCount) > length / sizeof(int32_t))
    {
        return false;
    }

    m_Types.clear();
    m_Types.reserve(typeCount);
    for (int i = 0; i < typeCount; ++i)
    {
        m_Types.emplace_back(nullptr);
        if (!m_Types[i].ReadType(iterator, end))
            return false;
    }

    int32_t objectCount = 0;
    if (!ReadHeaderCacheChecked(objectCount, iterator, end) || objectCount < 0 || static_cast<size_t>(objectCount) > length / sizeof(int32_t))
    {
        return false;
    }

    for (int i = 0; i < objectCount; i++)
    {
        Read4Alignment(data, iterator, end);

        int64_t fileID = 0;
        ObjectInfo value;
        if (!ReadHeaderCacheChecked(fileID, iterator, end) || !ReadHeaderCacheChecked(value.byteStart, iterator, end) ||
            !ReadHeaderCacheChecked(value.byteSize, iterator, end) || !ReadHeaderCacheChecked(value.typeID, iterator, end))
        {
            return false;
        }

        value.byteStart += dataOffset;

        m_Object[fileID] = value;
    }

    // PR-SE3a-refine -- read the ExternalReferences table for files
    // produced after the inner-header version bump. Files with
    // version == 0 (= every existing .zasset on disk before this PR)
    // skip this block entirely; m_Externals stays empty, so any PPtr
    // in those files resolves as a self-reference / null only -- which
    // is byte-identical to pre-PR behaviour because PPtr::Transfer
    // didn't serialize anything before this PR anyway.
    m_Externals.clear();
    if (version >= 1)
    {
        Read4Alignment(data, iterator, end);

        int32_t externalsCount = 0;
        if (!ReadHeaderCacheChecked(externalsCount, iterator, end) || externalsCount < 0)
            return false;

        // Bound externalsCount against remaining buffer, same defensive
        // pattern as typeCount / objectCount above. Each entry takes
        // at minimum 3 * uint32 (length-prefix words for empty
        // strings) + 1 byte (hasPath), i.e. 13 bytes.
        if (static_cast<size_t>(externalsCount) > static_cast<size_t>(end - iterator))
            return false;

        m_Externals.reserve(externalsCount);
        for (int32_t i = 0; i < externalsCount; ++i)
        {
            FileIdentifier ref;
            if (!ReadLengthPrefixedString(ref.guid, iterator, end))
                return false;
            if (!ReadLengthPrefixedString(ref.type, iterator, end))
                return false;

            uint8_t hasPath = 0;
            if (!ReadHeaderCacheChecked(hasPath, iterator, end))
                return false;
            if (hasPath)
            {
                if (!ReadLengthPrefixedString(ref.pathName, iterator, end))
                    return false;
            }

            m_Externals.push_back(std::move(ref));
        }
    }

    return iterator == end;
}

bool SerializedFile::ExtractObjectData(int64_t fileID, SerializedFile::ObjectData& data)
{
    ObjectMap::iterator iter = m_Object.find(fileID);
    if (iter == m_Object.end())
        return false;

    const ObjectInfo& info = iter->second;

    SerializedType& serializedType = m_Types[info.typeID];

    data.typeTree = serializedType.GetOldType();
    data.persistentTypeID = serializedType.GetPersistentTypeID();

    if (info.byteSize > 0)
    {
        CachedReader cache;
        cache.InitRead(*m_ReadFile, (m_ReadOffset + info.byteStart).Cast<size_t>(), info.byteSize);

        data.data.resize(info.byteSize);
        cache.Read(data.data.data(), info.byteSize);
        cache.End();
    }

    return true;
}