#include "AssetManager.h"

#include "Runtime/BaseClasses/IPPtrResolver.h"
#include "Runtime/BaseClasses/Object.h"
#include "Runtime/BaseClasses/PPtr.h"
#include "Runtime/BaseClasses/Type.h"
#include "Runtime/BaseClasses/TypeManager.h"
#include "Runtime/Core/Serialize/SerializationCaching/FileCacherRead.h"
#include "Runtime/Core/Serialize/SerializationCaching/FileCacherWrite.h"
#include "Runtime/Core/Serialize/SerializedFile.h"
#include "Runtime/Core/Serialize/SerializedFilePPtrResolver.h"
#include "Runtime/Core/Serialize/SerializedObjectIdentifier.h"
#include "Runtime/Core/YamlSerialize/YamlObjectGraph.h"
#include "Runtime/Project/ProjectInfo.h"
#include "Runtime/Resource/Config/ConfigManager.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#if defined(_MSC_VER)
    #include <excpt.h>
#endif

namespace
{
    // -------------------------------------------------------------------------
    // P2 #6 -- deterministic GUID derivation for binary `.zasset` files.
    //
    // Mirrors ScriptRegistry::DeterministicGuidFromPath byte-for-byte (same
    // FNV-1a 64-bit primitive, same dual-seed scheme producing 128 bits, same
    // lower-case hex serialisation). We deliberately duplicate the four lines
    // rather than depend on ScriptRegistry from this layer -- ScriptRegistry
    // owns text-source identity, AssetManager owns binary-asset identity, and
    // dragging in ScriptRegistry as a Runtime-on-Runtime engine-system
    // dependency would couple two otherwise-independent subsystems.
    //
    // The seed is the file's POSIX-style absolute path lower-cased on Windows
    // (NTFS is case-insensitive). Lower-casing avoids "Foo.zasset" vs
    // "foo.zasset" producing different GUIDs after a remote checkout on a
    // case-sensitive filesystem.
    //
    // This GUID is what gets stamped into the AssetFileHeader prefix when the
    // caller of WriteObjectToDiskThreadSafe doesn't supply one explicitly.
    // On re-write it stays identical (path -> hash is deterministic), so
    // AssetRegistry's GUID->path index doesn't churn between editor sessions.
    constexpr uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ULL;
    constexpr uint64_t kFnvPrime = 0x100000001b3ULL;

    uint64_t FnvHash64(const uint8_t* data, size_t len, uint64_t seed)
    {
        uint64_t h = seed;
        for (size_t i = 0; i < len; ++i)
        {
            h ^= static_cast<uint64_t>(data[i]);
            h *= kFnvPrime;
        }
        return h;
    }

    std::string ToHex64(uint64_t v)
    {
        static const char digits[] = "0123456789abcdef";
        char buf[16];
        for (int i = 15; i >= 0; --i)
        {
            buf[i] = digits[v & 0xF];
            v >>= 4;
        }
        return std::string(buf, 16);
    }

    std::string DeterministicGuidFromPath(const std::filesystem::path& path)
    {
        std::string s = path.lexically_normal().generic_string();
#if defined(_WIN32)
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(s.data());
        const size_t n = s.size();
        const uint64_t hi = FnvHash64(bytes, n, kFnvOffsetBasis);
        const uint64_t lo = FnvHash64(bytes, n, hi ^ 0x9e3779b97f4a7c15ULL);
        return ToHex64(hi) + ToHex64(lo);
    }

}  // anonymous namespace

namespace
{
#if defined(_MSC_VER)
    bool TryReadSerializedObjectWithSeh(SerializedFile* serialized_file, Object* object)
    {
        __try
        {
            serialized_file->ReadObject(1, *object);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
#endif

    bool TryReadSerializedObjectSafely(SerializedFile& serialized_file, Object& object, const std::filesystem::path& path)
    {
#if defined(_MSC_VER)
        if (!TryReadSerializedObjectWithSeh(&serialized_file, &object))
        {
            LOG_ERROR(ZAsset, "Exception while reading asset '{}', treating it as incompatible/legacy data", path.generic_string());
            return false;
        }
        return true;
#else
        try
        {
            serialized_file.ReadObject(1, object);
            return true;
        }
        catch (const std::exception& exception)
        {
            LOG_ERROR(ZAsset, "Exception while reading asset '{}': {}", path.generic_string(), exception.what());
            return false;
        }
        catch (...)
        {
            LOG_ERROR(ZAsset, "Unknown exception while reading asset '{}', treating it as incompatible/legacy data", path.generic_string());
            return false;
        }
#endif
    }
}  // namespace

std::filesystem::path AssetManager::GetFullPath(const eastl::string& relative_path) const
{
    const std::filesystem::path rel(relative_path.c_str());
    if (rel.is_absolute())
    {
        return std::filesystem::absolute(rel).lexically_normal();
    }

    const auto& config = GET_SYSTEM(ConfigManager);
    const std::filesystem::path engine_path =
        std::filesystem::absolute(config->GetRootFolder() / rel).lexically_normal();

    std::error_code ec;
    if (std::filesystem::exists(engine_path, ec))
    {
        return engine_path;
    }

    const std::shared_ptr<ProjectInfo> project_info = GET_SYSTEM(ProjectInfo);
    if (project_info != nullptr)
    {
        const std::filesystem::path project_content = project_info->GetProjectContent();
        if (!project_content.empty())
        {
            const std::filesystem::path project_path =
                std::filesystem::absolute(project_content / rel).lexically_normal();
            if (std::filesystem::exists(project_path, ec))
            {
                return project_path;
            }
        }
    }

    return engine_path;
}

std::filesystem::path AssetManager::GetEditorResourcePath(const std::string& relative_path) const
{
    return std::filesystem::absolute(GET_SYSTEM(ConfigManager)->GetResourceFolder() / relative_path.c_str());
}

std::string AssetManager::GetAssetTypeName(const std::filesystem::path& asset_path) const
{
    if (!std::filesystem::exists(asset_path))
    {
        return {};
    }

    SerializedFile* serialized_file = MemoryManager::CreateObject<SerializedFile>();
    if (serialized_file->InitializeRead(asset_path, kCacheSize) != kSerializedFileLoadError_None)
    {
        MemoryManager::DestroyObject(serialized_file);
        return {};
    }

    const uint64_t persistent_type_id = serialized_file->GetObjectPersistentTypeID(1);
    MemoryManager::DestroyObject(serialized_file);
    if (persistent_type_id == Type::UndefinedPersistentTypeID)
    {
        return {};
    }

    RuntimeTypeArray& runtime_types = TypeManager::GetInstance().GetRuntimeTypes();
    for (uint32_t type_index = 0; type_index < runtime_types.count; ++type_index)
    {
        const Type* type = runtime_types.types[type_index];
        if (type != nullptr && type->assetTypeID == persistent_type_id)
        {
            return type->GetName();
        }
    }
    return {};
}

std::vector<std::filesystem::path>
AssetManager::GetAssetsByType(const std::string& asset_type, const std::filesystem::path& search_root) const
{
    // Default implementation: no registry available, so we fall back to a
    // directory walk. This is what runtime (RuntimeAssetManager) sees, but
    // it's also the EditorAssetManager fallback when the async scan is
    // still in flight at startup.
    std::vector<std::filesystem::path> result;
    if (search_root.empty())
    {
        return result;
    }
    std::error_code ec;
    if (!std::filesystem::exists(search_root, ec))
    {
        return result;
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator(search_root, ec))
    {
        if (ec)
        {
            break;
        }
        if (!entry.is_regular_file() || entry.path().extension() != ".zasset")
        {
            continue;
        }
        if (GetAssetTypeName(entry.path()) != asset_type)
        {
            continue;
        }
        result.push_back(entry.path());
    }
    return result;
}

Object* AssetManager::ReadObject(int32_t instanceID)
{
    // PR-SE3a-migrate -- demand-load an object referenced by PPtr.
    //
    // The runtime contract is: a non-zero InstanceID in PPtr<T>
    // either points at a heap-resident object (handled by ObjectManager
    // ::IDToPointer before we get here) OR at one that lives on disk
    // and hasn't been hydrated yet. When we land here it's the second
    // case. The ID was minted by GetInstanceIDFromPathAndFileID, so
    // Remapper has the (sfIndex, lfid) tuple we need.
    if (instanceID == 0)
        return nullptr;

    SerializedObjectIdentifier ident;
    if (!m_Remapper.InstanceIDToSerializedObjectIdentifier(instanceID, ident))
        return nullptr;

    const std::string& path_name = PathIDToPathNameInternal(static_cast<int>(ident.serializedFileIndex));
    if (path_name.empty())
        return nullptr;

    std::filesystem::path asset_path(path_name);
    std::error_code ec;
    if (!std::filesystem::exists(asset_path, ec))
        return nullptr;

    // Discover the persistent type stamped inside the .zasset so we
    // can ask TypeManager for the right Type* without needing the
    // caller to know the static T. Cheap: the SerializedFile open
    // path is already cached internally per stream.
    SerializedFile* sf = GetSerializedFile(static_cast<int>(ident.serializedFileIndex));
    if (sf == nullptr)
        return nullptr;

    const uint64_t persistent_type_id = sf->GetObjectPersistentTypeID(ident.localIdentifierInFile);
    if (persistent_type_id == Type::UndefinedPersistentTypeID)
        return nullptr;

    const Type* resolved_type = nullptr;
    RuntimeTypeArray& runtime_types = TypeManager::GetInstance().GetRuntimeTypes();
    for (uint32_t type_index = 0; type_index < runtime_types.count; ++type_index)
    {
        const Type* candidate = runtime_types.types[type_index];
        if (candidate != nullptr && candidate->assetTypeID == persistent_type_id)
        {
            resolved_type = candidate;
            break;
        }
    }
    if (resolved_type == nullptr)
        return nullptr;

    return ReadObject(asset_path, resolved_type);
}

bool AssetManager::TryGetIdentityForInstance(int32_t instanceID,
                                             std::filesystem::path& outPath,
                                             int64_t& outLocalIdentifierInFile) const
{
    if (instanceID == 0)
        return false;

    SerializedObjectIdentifier ident;
    // const_cast is harmless: Remapper's reverse lookup is read-only
    // semantically, but the underlying unordered_map::find isn't a
    // const member because we expose the same Remapper instance to
    // mutating callers (GetOrGenerateInstanceID).
    if (!const_cast<Remapper&>(m_Remapper).InstanceIDToSerializedObjectIdentifier(instanceID, ident))
        return false;

    const std::string& path_name = PathIDToPathNameInternal(static_cast<int>(ident.serializedFileIndex));
    if (path_name.empty())
        return false;

    outPath = std::filesystem::path(path_name);
    outLocalIdentifierInFile = ident.localIdentifierInFile;
    return true;
}

void AssetManager::GetAssetGuidAndType(const std::filesystem::path& asset_path,
                                       std::string& outGuid,
                                       std::string& outAssetType) const
{
    // Runtime fallback: read the on-disk header directly. Editor
    // override (EditorAssetManager) consults AssetRegistry instead --
    // an order-of-magnitude faster on warm projects but requires the
    // registry to be in scope. The base impl exists so the runtime-
    // only build still produces correct PPtr writes when the editor
    // tooling that generated the .zasset isn't present.
    outGuid.clear();
    outAssetType.clear();

    if (asset_path.empty() || !std::filesystem::exists(asset_path))
        return;

    SerializedFile* serialized_file = MemoryManager::CreateObject<SerializedFile>();
    if (serialized_file->InitializeRead(asset_path, kCacheSize) != kSerializedFileLoadError_None)
    {
        MemoryManager::DestroyObject(serialized_file);
        return;
    }
    outGuid = serialized_file->GetAssetGuid();
    outAssetType = serialized_file->GetAssetTypeName();
    MemoryManager::DestroyObject(serialized_file);
}

bool AssetManager::TryGetPathForGuid(const std::string& /*guid*/, std::filesystem::path& /*outPath*/) const
{
    // Runtime fallback: no GUID -> path index. The runtime build
    // currently doesn't ship one (AssetRegistry is editor-only); an
    // RuntimeAssetBundle-backed equivalent will be needed when the
    // runtime path actually starts loading PPtr-bearing assets, which
    // is post-SE3b. Returning false here causes ReaderHook to treat
    // every external PPtr as null, matching the documented "dangling
    // reference at runtime in standalone build" fallback.
    return false;
}

Object& AssetManager::ProduceSingletonAsset(const std::string& name,
                                            std::filesystem::path& path,
                                            bool allowSerializeAsText)
{
    int32_t id = GetInstanceIDFromPathAndFileID(path, 1);
    auto&& type = TypeManager::GetInstance().ClassNameToType(name.c_str());
    Object* ptr = PPtr<Object>(id);
    return *ptr;
}

int32_t AssetManager::GetInstanceIDFromPathAndFileID(const std::filesystem::path& path, int64_t localIdentifierInFile)
{
    SerializedObjectIdentifier identifier;
    identifier.serializedFileIndex = InsertPathNameInternal(path, true);
    identifier.localIdentifierInFile = localIdentifierInFile;
    return m_Remapper.GetOrGenerateInstanceID(identifier);
}

SerializedFile* AssetManager::GetSerializedFile(const std::filesystem::path& path, LockFlags lockedFlags)
{
    return GetSerializedFile(InsertPathNameInternal(path, true), lockedFlags);
}

SerializedFile* AssetManager::GetSerializedFile(int serializedFileIndex, LockFlags lockedFlags)
{
    if (serializedFileIndex == -1)
        return nullptr;

    StreamNameSpace& stream = GetStreamNameSpaceInternal(serializedFileIndex);
    return stream.stream;
}

void AssetManager::AddStream()
{
    m_Streams.push_back(StreamNameSpace());
    m_GlobalToLocalNameSpace.push_back(IDRemap());
    m_LocalToGlobalNameSpace.push_back(IDRemap());
}

StreamNameSpace& AssetManager::GetStreamNameSpaceInternal(int nameSpaceID)
{
    StreamNameSpace& nameSpace = m_Streams[nameSpaceID];

    if (nameSpace.stream)
        return nameSpace;

    const std::string& pathName = PathIDToPathNameInternal(nameSpaceID);

    nameSpace.stream = MemoryManager::CreateObject<SerializedFile>();
    nameSpace.loadError = nameSpace.stream->InitializeRead(pathName, kCacheSize);
    if (nameSpace.loadError != kSerializedFileLoadError_None)
    {
    }
    m_ActiveStreams.insert(nameSpaceID);
    return nameSpace;
}

void AssetManager::CleanupStream(uint32_t serializedFileIndex)
{
    StreamNameSpace& stream = m_Streams[serializedFileIndex];

    if (stream.stream)
    {
        stream.stream->Release();
        stream.stream = nullptr;
    }
    m_ActiveStreams.erase(serializedFileIndex);
}

int AssetManager::GetSerializedFileIndexFromPath(const std::filesystem::path& path)
{
    return InsertPathNameInternal(path, true);
}

int AssetManager::WriteFile(const std::filesystem::path& path, int serializedFileIndex, WriteData* writeData, int size, const std::string& explicit_guid)
{
    const std::filesystem::path parent_path = path.parent_path();
    if (!parent_path.empty())
    {
        std::filesystem::create_directories(parent_path);
    }

    CachedWriter writer;

    FileCacherWrite serializedFileWriter;
    if (!serializedFileWriter.InitWriteFile(path, kCacheSizeForWriting))
    {
        LOG_ERROR(ZAsset, "failed to open asset file for writing {}", path.generic_string());
        return -1;
    }

    writer.InitWrite(serializedFileWriter);

    SerializedFile* tempSerialize = MemoryManager::CreateObject<SerializedFile>();
    tempSerialize->InitializeWrite(writer);

    // P2 #6 -- stamp identity into the AssetFileHeader prefix that
    // WriteHeaderAndMetadata is about to emit. We only get here from
    // `WriteObjectsToDiskThreadSafe`, whose only callers are asset
    // pipelines / importers / scene serialisation -- all of them
    // produce real `.zasset` files where AssetRegistry needs a stable
    // GUID. If a caller already populated the in-memory object with an
    // identity we'd surface it here; today no such metadata exists on
    // Object, so we fall back unconditionally to the path-derived
    // deterministic GUID. asset_type is taken from the FIRST object's
    // runtime type name -- multi-object .zasset files are rare in the
    // current pipeline (typically one Texture/Mesh/Material per file)
    // and AssetRegistry only needs a coarse classifier here, not a
    // perfect one.
    // Phase 6 texture cook: an explicit GUID (the source asset's GUID) keeps a
    // cooked .zasset resolvable in a player build even though its on-disk path
    // (Intermediate/Cooked/<Platform>/...) differs from the source path.
    tempSerialize->SetAssetGuid(!explicit_guid.empty() ? explicit_guid : DeterministicGuidFromPath(path));
    if (size > 0 && writeData[0].objectPtr != nullptr)
    {
        const Type* t = writeData[0].objectPtr->GetType();
        if (t != nullptr && t->GetName() != nullptr)
        {
            tempSerialize->SetAssetTypeName(t->GetName());
        }
    }

    // PR-SE3a-migrate -- push a per-write-session PPtr resolver. All
    // objects this session writes are registered as local before the
    // first WriteObject so any PPtr<T> field that points at a sibling
    // in this same .zasset round-trips as (FileID=0, PathID=local).
    //
    // Cross-file PPtrs go through the writer hook installed below:
    // we look up the target's identity in the Remapper (it must have
    // been minted by GetInstanceIDFromPathAndFileID at some earlier
    // point -- editors that set a PPtr by-name go through that path
    // explicitly), then ask GetAssetGuidAndType() to fill the
    // FileIdentifier from AssetRegistry. The resolver dedups
    // identical FileIdentifiers via SerializedFile::AddExternalRef.
    AssetManager* self = this;
    auto writerHook = [self, &path](int32_t instanceID, FileIdentifier& outRef, int64_t& outPathID) -> bool {
        std::filesystem::path target_path;
        int64_t target_lfid = 0;
        if (!self->TryGetIdentityForInstance(instanceID, target_path, target_lfid))
            return false;

        // Self-reference safety net: if for any reason the target
        // resolves back to the file we're currently writing, bail
        // and let the resolver's local-map handle it instead. This
        // shouldn't happen in practice because RegisterLocalObject
        // intercepts self-refs before the hook fires, but the
        // belt-and-braces check costs one path comparison.
        std::error_code ec;
        if (std::filesystem::equivalent(target_path, path, ec))
            return false;

        std::string guid;
        std::string asset_type;
        self->GetAssetGuidAndType(target_path, guid, asset_type);

        outRef.guid = std::move(guid);
        outRef.type = std::move(asset_type);
        outRef.pathName = target_path.lexically_normal().generic_string();
        outPathID = target_lfid;
        return true;
    };

    SerializedFilePPtrResolver resolver(tempSerialize, std::move(writerHook));
    for (int i = 0; i < size; ++i)
    {
        Object* o = writeData[i].objectPtr;
        if (o == nullptr)
            continue;
        resolver.RegisterLocalObject(o->GetInstanceID(), writeData[i].localIdentifierInFile);
    }

    {
        ScopedPPtrResolver resolverScope(&resolver);
        for (int i = 0; i < size; i++)
        {
            Object* o = writeData[i].objectPtr;
            int64_t localIdentifierInFile = writeData[i].localIdentifierInFile;
            tempSerialize->WriteObject(*o, localIdentifierInFile);
        }
    }

    WriteInfo writeInfo;
    const bool finish_success = tempSerialize->FinishWriting(&writeInfo.headerOffset);
    MemoryManager::DestroyObject(tempSerialize);
    return finish_success ? 0 : -1;
}

Object* AssetManager::ReadObject(std::filesystem::path& path, const Type* type)
{
    // PR-SE3a-migrate -- if the same (path, lfid=1) tuple was already
    // hydrated earlier in this session, skip the open and reuse the
    // ObjectManager-registered instance. This is what makes PPtr
    // dereference O(1) on warm assets and what guarantees a stable
    // address for the editor inspector that holds a raw Object* into
    // it. Without this short-circuit, every PPtr deref would re-open
    // the .zasset and produce a new Object* -- a footgun the moment
    // anyone caches the pointer.
    const int32_t cached_id = GetInstanceIDFromPathAndFileID(path, 1);
    if (cached_id != 0)
    {
        if (Object* cached = GET_SYSTEM(ObjectManager)->IDToPointer(cached_id))
            return cached;
    }

    CachedReader reader;

    FileCacherRead serializedFileReader(path, kCacheSize);

    SerializedFile* tempSerialize = MemoryManager::CreateObject<SerializedFile>();
    if (tempSerialize->InitializeRead(path, kCacheSize) != kSerializedFileLoadError_None)
    {
        MemoryManager::DestroyObject(tempSerialize);
        return nullptr;
    }

    // Produce with the persistent instance ID so the object lands in
    // ObjectManager's table at the same key future PPtrs (which the
    // Remapper will mint identically via GetInstanceIDFromPathAndFileID)
    // will look up. Passing 0 here -- as the pre-PR-SE3a-migrate code
    // did -- meant ReadObjectFromPersistentManager could never find
    // the object on a second visit, and PPtr<T>::operator T*() would
    // return nullptr after the first deref dropped the local handle.
    Object* obj = GET_SYSTEM(ObjectManager)->Produce(type, cached_id);
    if (obj == nullptr)
    {
        MemoryManager::DestroyObject(tempSerialize);
        return nullptr;
    }

    // PR-SE3a-migrate -- push a per-read-session resolver scope so any
    // PPtr<T> deserialized inside the object round-trips through the
    // local-pathID map. Self-refs resolve via RegisterLocalObject;
    // cross-file refs flow through the reader hook installed below.
    //
    // ReadObject below uses PathID=1 (the canonical single-asset
    // .zasset layout); we register the produced instanceID against
    // that PathID so a self-referential PPtr resolves back to `obj`.
    // Multi-object reads (Prefab / Scene) need a richer registration
    // loop -- to be added when those code paths actually start
    // emitting PPtrs (PR-SE3c+).
    AssetManager* self = this;
    auto readerHook = [self](const FileIdentifier& ref, int64_t pathID) -> int32_t {
        // Path-based fallback first: Unity-1:1 prefers GUID, but
        // version-1 files written by this very PR may legitimately
        // carry an empty GUID slot when AssetRegistry was warming up
        // at write time. The pathName slot is always populated by
        // our writer hook, so it's the more reliable identifier in
        // the short term. Once SE3b lands and ShaderRegistry
        // guarantees GUID coverage we'll flip the priority.
        std::filesystem::path target_path;
        if (!ref.pathName.empty())
        {
            std::error_code ec;
            std::filesystem::path candidate(ref.pathName);
            if (std::filesystem::exists(candidate, ec))
                target_path = std::move(candidate);
        }
        if (target_path.empty() && !ref.guid.empty())
        {
            std::filesystem::path resolved;
            if (self->TryGetPathForGuid(ref.guid, resolved))
                target_path = std::move(resolved);
        }
        if (target_path.empty())
            return 0;

        const int32_t id = self->GetInstanceIDFromPathAndFileID(target_path, pathID);
        // Important: we DO NOT call ReadObject(id) here. Lazy
        // instantiation is the whole point of PPtr -- consumers
        // that dereference via PPtr<T>::operator T*() trigger the
        // ObjectManager fallthrough, which calls
        // ReadObjectFromPersistentManager -> AssetManager::ReadObject(id),
        // which we wired above. Eager loading at deserialize time
        // would defeat the lazy contract and risk loading deps for
        // a PPtr the consumer never dereferences.
        return id;
    };

    SerializedFilePPtrResolver resolver(tempSerialize, {}, std::move(readerHook));
    resolver.RegisterLocalObject(obj->GetInstanceID(), 1);

    bool readOk = false;
    {
        ScopedPPtrResolver resolverScope(&resolver);
        readOk = TryReadSerializedObjectSafely(*tempSerialize, *obj, path);
    }

    if (!readOk)
    {
        MemoryManager::DestroyObject(obj);
        MemoryManager::DestroyObject(tempSerialize);
        return nullptr;
    }

    MemoryManager::DestroyObject(tempSerialize);
    return obj;
}

void AssetManager::UnloadStream(const std::filesystem::path& path)
{
    int nameSpaceID = InsertPathNameInternal(path, false);
    if (nameSpaceID == -1)
        return;

    CleanupStream(nameSpaceID);
    m_GlobalToLocalNameSpace[nameSpaceID].clear();
    m_LocalToGlobalNameSpace[nameSpaceID].clear();
}

bool AssetManager::WriteObjectToDiskThreadSafe(const std::filesystem::path& path, Object& object)
{
    Object* objects[] = {&object};
    return WriteObjectsToDiskThreadSafe(path, objects, nullptr, 1);
}

bool AssetManager::WriteObjectToDiskWithGuid(const std::filesystem::path& path, Object& object, const std::string& guid)
{
    int serializedFileIndex = GetSerializedFileIndexFromPath(path);
    if (serializedFileIndex == -1)
        return false;

    WriteData writeData;
    writeData.objectPtr = &object;
    writeData.localIdentifierInFile = 1;
    return WriteFile(path, serializedFileIndex, &writeData, 1, guid) == 0;
}

bool AssetManager::WriteObjectsToYaml(const std::filesystem::path& path, Object** objects, const int64_t* identifiers, size_t count)
{
    const std::filesystem::path parent_path = path.parent_path();
    if (!parent_path.empty())
    {
        std::error_code ec;
        std::filesystem::create_directories(parent_path, ec);
    }

    // Same writer hook as the binary WriteFile path: translate a cross-file
    // InstanceID into an external (guid/type/path, pathID) reference.
    AssetManager* self = this;
    auto writerHook = [self, &path](int32_t instanceID, FileIdentifier& outRef, int64_t& outPathID) -> bool {
        std::filesystem::path target_path;
        int64_t target_lfid = 0;
        if (!self->TryGetIdentityForInstance(instanceID, target_path, target_lfid))
            return false;

        std::error_code ec;
        if (std::filesystem::equivalent(target_path, path, ec))
            return false;

        std::string guid;
        std::string asset_type;
        self->GetAssetGuidAndType(target_path, guid, asset_type);

        outRef.guid = std::move(guid);
        outRef.type = std::move(asset_type);
        outRef.pathName = target_path.lexically_normal().generic_string();
        outPathID = target_lfid;
        return true;
    };

    std::vector<ZYaml::ObjectGraphEntry> entries;
    entries.reserve(count);
    for (size_t i = 0; i < count; ++i)
    {
        if (objects[i] == nullptr)
            continue;
        ZYaml::ObjectGraphEntry e;
        e.object = objects[i];
        e.fileID = (identifiers != nullptr) ? identifiers[i] : static_cast<int64_t>(i + 1);
        entries.push_back(e);
    }

    eastl::string yaml;
    if (!ZYaml::WriteObjectGraph(entries, yaml, std::move(writerHook)))
        return false;

    std::ofstream output(path, std::ios::binary);
    if (!output)
    {
        LOG_ERROR(ZAsset, "failed to open yaml asset for writing {}", path.generic_string());
        return false;
    }
    output.write(yaml.c_str(), static_cast<std::streamsize>(yaml.size()));
    return output.good();
}

bool AssetManager::ReadObjectsFromYaml(const std::filesystem::path& path, std::vector<std::pair<int64_t, Object*>>& out)
{
    out.clear();

    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        LOG_ERROR(ZAsset, "failed to open yaml asset for reading {}", path.generic_string());
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());

    // Same reader hook as the binary ReadObject path: map an external
    // (guid/path, pathID) reference back to a runtime InstanceID (lazily).
    AssetManager* self = this;
    auto readerHook = [self](const FileIdentifier& ref, int64_t pathID) -> int32_t {
        std::filesystem::path target_path;
        if (!ref.pathName.empty())
        {
            std::error_code ec;
            std::filesystem::path candidate(ref.pathName);
            if (std::filesystem::exists(candidate, ec))
                target_path = std::move(candidate);
        }
        if (target_path.empty() && !ref.guid.empty())
        {
            std::filesystem::path resolved;
            if (self->TryGetPathForGuid(ref.guid, resolved))
                target_path = std::move(resolved);
        }
        if (target_path.empty())
            return 0;

        return self->GetInstanceIDFromPathAndFileID(target_path, pathID);
    };

    std::vector<ZYaml::ObjectGraphEntry> entries;
    if (!ZYaml::ReadObjectGraph(text.c_str(), entries, std::move(readerHook)))
        return false;

    out.reserve(entries.size());
    for (const ZYaml::ObjectGraphEntry& e : entries)
        out.emplace_back(e.fileID, e.object);
    return true;
}

bool AssetManager::WriteObjectsToDiskThreadSafe(const std::filesystem::path& path, Object** objects, const int64_t* identifiers, size_t count, TransferInstructionFlags transferFlags)
{
    int serializedFileIndex = GET_SYSTEM(AssetManager)->GetSerializedFileIndexFromPath(path);
    if (serializedFileIndex == -1)
        return false;

    std::vector<WriteData> writeData;
    writeData.resize(count);
    for (int i = 0; i < count; i++)
    {
        Object* o = objects[i];
        writeData[i].objectPtr = o;
        if (identifiers != nullptr)
            writeData[i].localIdentifierInFile = identifiers[i];
        else
            writeData[i].localIdentifierInFile = i + 1;
    }
    return GET_SYSTEM(AssetManager)->WriteFile(path, serializedFileIndex, writeData.data(), static_cast<int>(count)) == 0;
}
