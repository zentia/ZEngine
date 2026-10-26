#pragma once

#include <filesystem>
#include <string>

// =====================================================================================
// SceneCook -- the player's copy of a scene (doc/PrefabSystem_Design.md §7.5 / §9.4.4).
// -------------------------------------------------------------------------------------
// An editor scene carries editor-only bookkeeping next to its GameObjects -- today the
// PrefabInstance objects that remember which prefab a subtree came from and what was
// overridden. The GameObject tree itself is already concrete (PrefabSceneBinding keeps
// the instantiated, overridden objects in the scene), so "flattening" a PrefabInstance
// for the player is dropping the bookkeeping object, not re-expanding anything.
//
// CookSceneFile writes the player copy: every object whose type is registered with
// REGISTER_CLASS_TRAITS(kTypeIsEditorOnly) is removed, and externals only those objects
// referred to (the source .prefab) are pruned. Text-level (ZYaml::StripObjectsFromGraph):
// no object is produced, so cooking a scene that is open in the editor does not disturb
// it. YAML (.scene) only -- the editor saves scenes as YAML; a binary level is copied
// unchanged and reported.
// =====================================================================================
struct SceneCookResult
{
    bool ok = false;
    bool stripped = false;  ///< false: the input was copied unchanged (not a YAML graph)
    int removed_objects = 0;
    int removed_externals = 0;
    int dangling_references = 0;  ///< player objects that pointed at a removed object (now null)
    std::string error;
};

namespace SceneCook
{
    /// True for objects of `type_name` that must not reach the player. Unknown type
    /// names are kept (they may be runtime types of a module this build lacks).
    bool IsEditorOnlyTypeName(const char* type_name);

    SceneCookResult CookSceneFile(const std::filesystem::path& source, const std::filesystem::path& cooked);
}  // namespace SceneCook
