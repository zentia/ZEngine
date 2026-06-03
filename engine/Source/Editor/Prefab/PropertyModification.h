#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/BaseClasses/PPtr.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"

#include <EASTL/string.h>
#include <cstdint>
#include <vector>

// =====================================================================================
// PropertyModification — Editor-only override record for the Prefab system.
// -------------------------------------------------------------------------------------
// One PropertyModification represents a single override applied by a PrefabInstance to
// a particular property of a particular target object (a GameObject or Component that
// was cloned from the source PrefabAsset).
//
// Mirrors Unity's `Editor/Src/Prefabs/PropertyModification.h` 1:1 in layout:
//
//     target          PPtr<Object>   The instance object the override applies to.
//                                    (NOT the source object — apply pipeline uses the
//                                     PrefabInstance.corresponding_source map to look
//                                     up the matching source object.)
//     property_path   eastl::string  Dot-and-bracket path inside the object, e.g.
//                                    "m_LocalPosition.x", "m_Components[2]", or
//                                    just "m_Name".
//     value           eastl::string  Encoded value for primitives and strings. The
//                                    encoder/decoder is part of PropertyPathResolver;
//                                    floats/ints/bools/enums use their textual repr.
//     object_reference PPtr<Object>  Used instead of `value` when the target field is
//                                    itself a PPtr<>/ObjectReference.
//
// PropertyModifications are stored on PrefabInstance and applied by `MergePrefabInto`
// (Phase 2b) after each per-object Transfer-from-source completes.
//
// IMPORTANT: keep this struct trivially-Transferable. Adding fields here means every
// existing .zasset on disk needs its schema_version bumped — coordinate with
// PrefabAsset::kCurrentSchemaVersion when doing so.
// =====================================================================================
struct PropertyModification
{
    DECLARE_SERIALIZE(PropertyModification)

    PPtr<Object> target;
    eastl::string property_path;
    eastl::string value;
    PPtr<Object> object_reference;

    /// Raw byte payload of the override, in StreamedBinary layout (host-endian).
    /// This is the value Apply/Revert actually splice into the target's serialised
    /// stream — `value` (the eastl::string above) is kept for editor display and
    /// future YAML round-trip but is NOT consulted by the apply pipeline.
    ///
    /// Stored as eastl::string because ZEngine's SerializeTraits only specialises
    /// std::vector for the basic-typed elements it knows about (int32/uint32/...
    /// — uint8_t is NOT in that set), so a `std::vector<uint8_t>` field wouldn't
    /// compile through TRANSFER. eastl::string accepts arbitrary byte content
    /// (it's not zero-terminator constrained on the wire) and is already
    /// fully covered by SerializeTraits.
    ///
    /// When `object_reference` is non-null, both `value` and `value_bytes` are
    /// ignored — the apply pipeline pulls the InstanceID from object_reference and
    /// writes a 4-byte int into the slot.
    eastl::string value_bytes;

    // Equality helpers used by Apply/Revert and by GeneratePropertyDiff (Phase 2b).
    static bool ComparePathAndTarget(const PropertyModification& lhs, const PropertyModification& rhs)
    {
        return lhs.target.GetInstanceID() == rhs.target.GetInstanceID() &&
               lhs.property_path == rhs.property_path;
    }

    static bool CompareValues(const PropertyModification& lhs, const PropertyModification& rhs)
    {
        return lhs.value == rhs.value &&
               lhs.value_bytes == rhs.value_bytes &&
               lhs.object_reference.GetInstanceID() == rhs.object_reference.GetInstanceID();
    }

    static bool CompareAll(const PropertyModification& lhs, const PropertyModification& rhs)
    {
        return ComparePathAndTarget(lhs, rhs) && CompareValues(lhs, rhs);
    }
};

template<class TransferFunction>
void PropertyModification::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(target, "target");
    transfer.Transfer(property_path, "property_path");
    transfer.Transfer(value, "value");
    transfer.Transfer(object_reference, "object_reference");
    transfer.Transfer(value_bytes, "value_bytes");
}

// Construction helpers matching Unity's `CreatePropertyModification(...)` overloads.
inline PropertyModification MakePropertyModification(const eastl::string& property_path,
                                                     const eastl::string& value,
                                                     PPtr<Object> target = PPtr<Object>())
{
    PropertyModification mod;
    mod.target = target;
    mod.property_path = property_path;
    mod.value = value;
    return mod;
}

inline PropertyModification MakePropertyModification(const eastl::string& property_path,
                                                     PPtr<Object> object_reference,
                                                     PPtr<Object> target = PPtr<Object>())
{
    PropertyModification mod;
    mod.target = target;
    mod.property_path = property_path;
    mod.object_reference = object_reference;
    return mod;
}
