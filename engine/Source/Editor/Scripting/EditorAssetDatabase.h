#pragma once

#include "Runtime/BaseClasses/Object.h"

#include <eastl/string.h>

#include <vector>

#include "EditorAssetDatabase.generated.h"

// Editor-VM only: asset lookup (Unity's `UnityEditor.AssetDatabase`).
//
// P1. Resolution and identity only -- the three things a tool script actually
// needs before it can do anything with an asset: does it exist, what is its
// absolute path, and what is its stable GUID.
//
// What is deliberately NOT here
// -----------------------------
// * No LoadAssetAtPath. Returning a live `Object*` to a script means handing
//   out a pointer into the asset cache with no ownership story on the script
//   side; the existing script-facing GameObject* pointers are borrowed for the
//   duration of one call, which does not work for an asset a script wants to
//   keep. Needs a handle type first.
// * No CreateAsset / SaveAssets / DeleteAsset. Writing assets from a script
//   must go through the undo system and the asset registry together, and the
//   undo system has no record type for an asset file (EditorHistory records
//   components and GameObjects, not files). A script that could create an
//   asset but not undo it is a trap.
//
// `FindAssets` landed in P4 once Converter<std::vector<T>> existed -- the P1
// note here said it was blocked on a container marshaller, and that is now
// gone.
//
// Paths are forward-slash absolute, matching EditorEnvironment.GetProjectRoot()
// so scripts can concatenate the two without worrying about separators.
ZCLASS()
class EditorAssetDatabase : public Object
{
    ZGENERATED_BODY(EditorAssetDatabase)

public:
    /// Absolute, forward-slash path for `asset_path`, which may be relative to
    /// the engine root or the project root (same resolution the loaders use).
    /// Returns "" when it resolves to nothing.
    ZFUNCTION()
    static eastl::string GetFullPath(const eastl::string& asset_path);

    /// Whether a file exists at `asset_path` (after the same resolution).
    ZFUNCTION()
    static bool Exists(const eastl::string& asset_path);

    /// Stable GUID of the asset, or "" when it has none. This is the identity
    /// PPtr references are written against, so it is what a script should log
    /// or compare rather than a path.
    ZFUNCTION()
    static eastl::string GetGuid(const eastl::string& asset_path);

    /// Asset type name as the pipeline records it (e.g. "AnimationAsset"), or
    /// "" when unknown.
    ZFUNCTION()
    static eastl::string GetAssetType(const eastl::string& asset_path);

    /// Inverse of GetGuid: absolute path of the asset with that GUID, or ""
    /// when the registry does not know it.
    ZFUNCTION()
    static eastl::string GetPathForGuid(const eastl::string& guid);

    // ---- search (P4) -------------------------------------------------------

    /// Registry-relative paths of every asset matching the filters. Each filter
    /// is ignored when empty, so FindAssets("", "", "") lists everything.
    ///
    ///   path_prefix    package path prefix, forward slashes, relative to the
    ///                  scan root -- e.g. "objects/character"
    ///   asset_type     exact type name, e.g. "AnimSkelMap"
    ///   name_substring matched against the file stem
    ///
    /// Returns a SNAPSHOT (see the Converter note in ScriptBinding.h): the
    /// array is a copy, so it neither reflects later registry changes nor
    /// writes back. Editor-only -- a player build carries no AssetRegistry and
    /// this returns empty there.
    ZFUNCTION()
    static std::vector<eastl::string> FindAssets(const eastl::string& path_prefix,
                                                 const eastl::string& asset_type,
                                                 const eastl::string& name_substring);

    /// How many assets match, without materialising the paths. Cheaper than
    /// FindAssets(...).length for a script that only wants to know "any?".
    ZFUNCTION()
    static int CountAssets(const eastl::string& path_prefix,
                           const eastl::string& asset_type,
                           const eastl::string& name_substring);
};
