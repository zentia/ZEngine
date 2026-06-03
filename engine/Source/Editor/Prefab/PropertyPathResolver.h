#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Serialize/TypeTree.h"

#include <EASTL/string.h>
#include <cstdint>
#include <vector>

// =====================================================================================
// PropertyPathResolver
// -------------------------------------------------------------------------------------
// Tokenises and validates textual property paths used by the Prefab override system,
// and locates the matching node inside an object's TypeTree.
//
// IMPORTANT — what this resolver does NOT do (and why):
//
//   It deliberately does NOT translate a path into a byte offset inside a serialised
//   stream. The reason: ZEngine's GenerateTypeTreeTransfer records ByteOffsets as
//   C++ field offsets (data - m_ObjectPtr), which do NOT line up with the serialised
//   StreamedBinary representation (vtables, padding, length-prefixed string/vector
//   encodings all break the equality). Unity solves this with the same primitive
//   ZEngine has — SafeBinaryRead — by streaming the object back in and intercepting
//   the matching field name path mid-read. Phase 2b implements that interceptor;
//   Phase 2a only needs the path-→-TypeTreeIterator lookup so callers can
//   discriminate "valid override path" from "stale/broken path on a missing field".
//
// Phase 2a contract — supported path syntax:
//
//     <field>                       e.g. "m_Name"
//     <field>.<subfield>            e.g. "m_LocalPosition.x"
//     <field>[<index>]              e.g. "m_Components[2]"
//     <field>[<index>].<subfield>   e.g. "m_Components[2].m_Enabled"
//     repeating any of the above to arbitrary nesting depth.
//
// NOT yet supported (deferred to Phase 3+):
//     - "managedReferences[<refid>].xxx" (SerializeReference dynamic dispatch)
//     - "m_Dict.<key>" (string-keyed maps; TypeTree models them as KV-pair arrays)
//
// Resolver is purely Editor-side. Runtime consumers never see PropertyPaths.
// =====================================================================================

namespace prefab_editor
{

    // One step inside a PropertyPath. Walking the path consumes these in order.
    struct PathToken
    {
        enum class Kind
        {
            Field,  // ".m_LocalPosition", or top-level "m_Name"
            Index,  // "[2]"
        };

        Kind kind {Kind::Field};
        eastl::string field;       // populated when kind == Field
        int32_t array_index {-1};  // populated when kind == Index
    };

    /// Tokenises "m_Components[2].m_Enabled" into [Field("m_Components"), Index(2),
    /// Field("m_Enabled")]. Returns false on malformed input (mismatched brackets,
    /// empty field, non-decimal index, ...).
    bool TokenizePropertyPath(const eastl::string& path, std::vector<PathToken>& out_tokens);

    /// Pairs a TypeTree describing field layout with optional StreamedBinary bytes.
    /// Phase 2a only fills `type_tree`; `bytes` stays empty until Phase 2b lifts the
    /// known issue in MemoryCacheWriter (m_Memory is held by-value, so the existing
    /// TransferUtility::WriteObjectToVector helper silently drops its output).
    struct SerialisedObject
    {
        TypeTree type_tree;
        std::vector<uint8_t> bytes;

        bool empty() const { return bytes.empty(); }
    };

    /// Walks `object` once with GenerateTypeTreeTransfer to populate `out.type_tree`.
    /// Returns false if `object` is null. The TypeTree retains a strong reference to its
    /// shareable data, so it remains valid past this function's return.
    ///
    /// Phase 2a deliberately does NOT capture the StreamedBinary byte stream — see the
    /// comment on SerialisedObject above. Phase 2b will add a fixed memory writer and
    /// produce both halves in one pass.
    bool SerialiseObject(Object* object, SerialisedObject& out);

    /// Walks `type_tree` from root following `tokens`, returning the leaf iterator on
    /// success. On failure (token doesn't match, array index out of meaningful range,
    /// path runs past the tree) returns a null iterator (.IsNull()).
    ///
    /// IMPORTANT: arrays in the TypeTree are modelled as
    ///   <array_node TypeFlags=kFlagIsArray>
    ///     ├─ "size"    int
    ///     └─ "data"    <element-type-tree>
    /// so an Index token causes the resolver to descend through the array's element
    /// subtree (the "data" child), reusing it for every element index. Array bound
    /// checking is the caller's responsibility (Phase 2b cross-references the index
    /// against the actual `size` value pulled from the byte stream during apply).
    TypeTreeIterator LocateInTypeTree(const TypeTree& type_tree, const std::vector<PathToken>& tokens);

    /// Convenience overload: tokenise + locate in one call.
    TypeTreeIterator LocateInTypeTree(const TypeTree& type_tree, const eastl::string& path);

}  // namespace prefab_editor
