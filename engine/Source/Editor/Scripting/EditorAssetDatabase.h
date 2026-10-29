#pragma once

#include "Runtime/BaseClasses/Object.h"

#include <eastl/string.h>

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
//   must go through the undo system and the asset registry together, and
//   EditorHistory cannot yet record a scripted change (see its header). A
//   script that could create an asset but not undo it is a trap.
// * No FindAssets glob. ZFUNCTION cannot marshal a container (ts_type_for
//   rejects std::vector), so a search API would have to be counted-access over
//   cached results -- i.e. stateful. Deferred until there is a real caller.
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
};
