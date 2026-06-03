#include "TransferUtility.h"

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/BaseClasses/Type.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Serialize/SerializationCaching/MemoryCacheWriter.h"
#include "Runtime/Core/Serialize/TransferFunctions/StreamedBinaryRead.h"
#include "Runtime/Core/Serialize/TransferFunctions/StreamedBinaryWrite.h"
#include "Runtime/Resource/Asset/AssetManager.h"

void TransferUtility::WriteObjectToVector(const Object& object, std::vector<uint8_t>& data)
{
    // Reset to drop any pre-existing payload — the caller-facing contract is
    // "fill `data` with the object's serialized bytes", not "append".
    data.clear();

    MemoryCacheWriter memoryCache(data);
    StreamedBinaryWrite writeStream;
    CachedWriter& writeCache = writeStream.Init();

    writeCache.InitWrite(memoryCache);

    const_cast<Object&>(object).VirtualRedirectTransfer(writeStream);

    // Trim the buffer down to the real payload length. Without this call, `data`
    // would still carry MemoryCacheWriter's last preallocated capacity (rounded up
    // to kCacheSize), not the logical end of the write — which would corrupt every
    // downstream `data.size()`-based reader (SafeBinaryRead, MemoryCacheReader, etc.).
    writeCache.CompleteWriting();
}

bool TransferUtility::ReadObjectFromVector(Object& object, std::vector<uint8_t>& data)
{
    if (data.empty())
    {
        return false;
    }

    MemoryCacheReader memoryCache(data);
    StreamedBinaryRead readStream;
    CachedReader& readCache = readStream.Init(kNoTransferInstructionFlags);

    readCache.InitRead(memoryCache, /*position=*/0, /*readSize=*/data.size());

    object.VirtualRedirectTransfer(readStream);

    readCache.End();
    return true;
}

Object* TransferUtility::CloneObjectViaSerialization(const Object& source)
{
    // 1) Allocate a fresh object of the same concrete type. Produce(type, 0) is
    //    the deferred-id path: it skips IDToPointer registration so we can assign
    //    a heap id with AllocateAndAssignInstanceID below.
    const Type* concrete_type = source.GetType();
    if (concrete_type == nullptr)
    {
        LOG_ERROR(ZEngine, "TransferUtility::CloneObjectViaSerialization: source has no concrete Type");
        return nullptr;
    }

    auto&& object_manager = GET_SYSTEM(ObjectManager);
    Object* clone = object_manager->Produce(concrete_type, /*instanceID=*/0);
    if (clone == nullptr)
    {
        LOG_ERROR(ZEngine, "TransferUtility::CloneObjectViaSerialization: factory for type '{}' returned null", concrete_type->GetName());
        return nullptr;
    }

    // 2) Round-trip serialise the source into a buffer, then read it back into the
    //    fresh clone. This implicitly deep-copies every plain field; nested PPtr<>s
    //    keep pointing at the original targets (caller must remap if needed).
    std::vector<uint8_t> bytes;
    WriteObjectToVector(source, bytes);
    if (bytes.empty())
    {
        // Empty serialisation usually means the object's Transfer is a no-op or the
        // object is in an uninitialised state. Either way the clone is just an
        // empty shell; not a hard error.
        LOG_WARNING(ZEngine, "TransferUtility::CloneObjectViaSerialization: source serialised to 0 bytes (type '{}')", concrete_type->GetName());
    }
    else if (!ReadObjectFromVector(*clone, bytes))
    {
        LOG_ERROR(ZEngine, "TransferUtility::CloneObjectViaSerialization: round-trip read failed for type '{}'", concrete_type->GetName());
        return nullptr;
    }

    // 3) Now that the clone has its serialised state, give it a fresh negative
    //    InstanceID and register it. We deliberately do this *after* the read so
    //    the read can't accidentally clobber m_InstanceID (the field isn't part of
    //    Transfer(), but defending against future regressions is cheap).
    object_manager->AllocateAndAssignInstanceID(clone);
    return clone;
}

SerializedFile* TransferUtility::OpenSerializedFileAndReadObjectData(const std::filesystem::path& path, int64_t fileID, SerializedFile::ObjectData& objData)
{
    SerializedFile* stream = GET_SYSTEM(AssetManager)->GetSerializedFile(path);
    if (stream == nullptr)
        return nullptr;

    if (!stream->ExtractObjectData(fileID, objData))
    {
        GET_SYSTEM(AssetManager)->UnloadStream(path);
        return nullptr;
    }
    return stream;
}
