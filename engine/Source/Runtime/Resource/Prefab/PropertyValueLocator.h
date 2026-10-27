#pragma once

#include "Runtime/Resource/Prefab/PropertyPathResolver.h"
#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Serialize/TypeTree.h"

#include <EASTL/string.h>
#include <cstdint>
#include <vector>

// =====================================================================================
// PropertyValueLocator — locates a single property's bytes inside an object's
// StreamedBinary representation, and can mutate them in place.
// -------------------------------------------------------------------------------------
// Phase 2b-3 deliverable.
//
// What this header lifts (vs. Phase 2a's PropertyPathResolver):
//
//   PropertyPathResolver only walked the TypeTree. This module adds the byte-stream
//   walker on top: given the same TypeTree plus the actual StreamedBinary bytes,
//   `LocatePropertyInStream` returns the `(offset, size)` of the leaf field. That's
//   exactly the primitive Apply needs to write a per-instance override into a
//   freshly serialised buffer, and that Revert needs to read the source's value
//   from a byte stream.
//
// Algorithmic basis:
//   The byte-walker is a direct port of `SafeBinaryRead::Walk` (see
//   Runtime/Core/Serialize/TransferFunctions/SafeBinaryRead.cpp), reduced to the
//   subset needed here (no version conversion, no array fast-path divergence —
//   we always traverse element-by-element when an index token is in flight).
//
// =====================================================================================

namespace prefab
{

    /// Result of a successful property-locate operation.
    ///
    /// `byte_offset` is relative to the start of the StreamedBinary buffer that was
    /// passed to the locator (NOT relative to the SerializedFile, NOT a C++ field
    /// offset). `byte_size` is -1 when the leaf is a variable-size aggregate
    /// (vector/string/struct with variable children). Callers wanting to overwrite a
    /// scalar always get a fixed positive size; callers wanting to inspect a complex
    /// leaf read sub-fields directly off the underlying TypeTree.
    struct PropertyLocation
    {
        size_t byte_offset {0};
        int64_t byte_size {-1};
        TypeTreeIterator leaf_node;

        bool valid() const { return !leaf_node.IsNull() && byte_size >= 0; }
    };

    /// Walk `bytes` guided by `type_tree`, descending into the field path described by
    /// `tokens`. On full match, returns true and fills `out`. On any failure (path
    /// doesn't exist, array index out of range against the actual `size` recorded in
    /// the stream, malformed stream, leaf is variable-size and not addressable), returns
    /// false.
    ///
    /// The function is read-only over `bytes`; mutation happens via `OverwriteScalar`.
    bool LocatePropertyInStream(
        const TypeTree& type_tree,
        const std::vector<uint8_t>& bytes,
        const std::vector<PathToken>& tokens,
        PropertyLocation& out);

    /// Convenience: tokenise `path` then locate.
    bool LocatePropertyInStream(
        const TypeTree& type_tree,
        const std::vector<uint8_t>& bytes,
        const eastl::string& path,
        PropertyLocation& out);

    /// Explain WHY `path` does not resolve against `type_tree` — for logs and test
    /// failures. Pure diagnosis over the TypeTree (no stream bytes needed, so it also
    /// works when the stream itself is what is broken). Returns a one-line message:
    ///
    ///   * "resolves" — every segment exists in the type tree (a failure of the real
    ///     locator then means the STREAM is malformed or an array index is out of range);
    ///   * "no field 'x' under 'a.b' (available: m_p, m_q, ...)" — the misspelt-name case,
    ///     which is by far the commonest: serialized names are NOT the C++ member names
    ///     (m_ScriptGuid is serialized as "m_script_guid");
    ///   * "'a.b' is an array: ... " — an index was expected / a field was given;
    ///   * "cannot index into 'a.b'" — "[n]" on something that is not an array.
    ///
    /// Note the locator can fail on a path this reports as "resolves": it cannot step
    /// over a variable-size array element ("shared_materials[0]"), because it must walk
    /// the elements before it. Override the whole array instead.
    eastl::string ExplainPathFailure(const TypeTree& type_tree, const std::vector<PathToken>& tokens);
    eastl::string ExplainPathFailure(const TypeTree& type_tree, const eastl::string& path);

    /// Overwrite `out_bytes[loc.byte_offset .. loc.byte_offset + loc.byte_size)` with
    /// `new_value` (which must already be in the correct StreamedBinary
    /// little-endian-or-platform-native layout for the leaf's type). Caller is
    /// responsible for the value being the right type and width — the locator does NOT
    /// do conversion. Returns false if the location is invalid or the size mismatches.
    bool OverwriteScalarBytes(
        std::vector<uint8_t>& out_bytes,
        const PropertyLocation& loc,
        const void* new_value,
        size_t new_value_size);

}  // namespace prefab
