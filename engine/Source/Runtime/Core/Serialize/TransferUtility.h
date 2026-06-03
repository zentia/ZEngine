#pragma once

#include "Runtime/Core/Serialize/SerializedFile.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "SerializationCaching/MemoryCacheReader.h"
#include "TransferFunctions/SafeBinaryRead.h"

#include <vector>

class Object;

class TransferUtility
{
public:
    /// Serialise `object` into `data` using the StreamedBinary writer. Caller-side
    /// contract: `data` is overwritten (not appended); on return its size is the
    /// exact byte length of the encoded object. Phase 2b-1 fixed three latent bugs
    /// in the underlying MemoryCacheWriter / WriteObjectToVector path; see history
    /// in MemoryCacheWriter.h.
    static void WriteObjectToVector(const Object& object, std::vector<uint8_t>& data);

    /// Deserialise StreamedBinary `data` back into `object`, in place. The Object
    /// must already be a fresh instance of the *correct concrete type* — typically
    /// allocated via ObjectManager::NewObject<DerivedType>(); ReadObjectFromVector
    /// only fills its serialised fields and does NOT call Produce/factory dispatch.
    ///
    /// Pairs with WriteObjectToVector for in-memory deep cloning. Returns true on
    /// success, false if the buffer is empty or a stream error is detected.
    static bool ReadObjectFromVector(Object& object, std::vector<uint8_t>& data);

    /// Allocate a fresh Object of the same concrete type as `source`, then copy its
    /// serialised state through a StreamedBinary write/read round-trip. Returns the
    /// newly-allocated Object (already registered with ObjectManager and assigned a
    /// new InstanceID), or nullptr on failure.
    ///
    /// NOTE: nested PPtr<>s inside the cloned object still point at the original
    /// targets (intentional — Phase 3 will add a remap pass for the prefab-instance
    /// case where every contained PPtr must be redirected to its clone counterpart).
    static Object* CloneObjectViaSerialization(const Object& source);

    static SerializedFile* OpenSerializedFileAndReadObjectData(const std::filesystem::path& path, int64_t fileID, SerializedFile::ObjectData& objData);
};
