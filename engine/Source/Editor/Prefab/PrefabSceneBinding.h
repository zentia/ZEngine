#pragma once

#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <vector>

class GameObject;
class Level;
class PrefabInstance;

// =====================================================================================
// PrefabSceneBinding — keeps editor PrefabInstance bookkeeping alive across scene
// save / load (doc/PrefabSystem_Design.md §9.4).
// -------------------------------------------------------------------------------------
// The scene file is written and read by Runtime's Level, which knows nothing about
// the editor-only PrefabInstance. Install() puts three Level serialization hooks in
// place:
//
//   * save   — every PrefabInstance registered for the level whose root GameObject
//              is still in the level is written into the same file, so its PPtrs to
//              the level's GameObjects / Components resolve to local fileIDs.
//   * load   — every PrefabInstance read back is retargeted from the GameObjects
//              read from the file to the live GameObjects Level built from them
//              (components are shared between the two, GameObjects are not), then
//              registered for the level.
//   * unload — the level's registrations are dropped.
//
// PlaceInLevel() is the "drop a prefab into the scene" step: it moves the cloned
// subtree of a fresh InstantiateAsPrefab() result into a Level the same way load
// does and registers the instance.
//
// Ownership: the registry does not own PrefabInstance objects. Like the GameObjects
// Level::load reads from the file (which it copies and then leaves in place), the
// instances are left to the ObjectManager.
// =====================================================================================
class PrefabSceneBinding
{
public:
    /// Installs the Level serialization hooks. Idempotent. Called from
    /// RegisterEditor(); headless tests call it themselves.
    static void Install();

    /// Removes the hooks (only the ones this class installed) and clears the registry.
    static void Uninstall();

    /// Moves the cloned subtree of `instance` (as returned by
    /// PrefabUtilityEditor::InstantiateAsPrefab) into `level`: one live GameObject per
    /// cloned GameObject, sharing the cloned components; the instance's GameObject
    /// references are retargeted to the live objects; the instance is registered for
    /// `level` and the level is marked dirty. Returns false (and changes nothing) if
    /// `instance` has no root.
    static bool PlaceInLevel(Level& level, PrefabInstance* instance);

    /// "Drop a prefab into the scene": PrefabUtilityEditor::InstantiateAsPrefab(path)
    /// followed by PlaceInLevel. Returns the placed instance (its root GameObject is a
    /// live object of `level`), or nullptr if the prefab cannot be loaded / placed.
    /// The content-browser drop (EditorScenePlacement) goes through here; undo,
    /// parenting and selection are the caller's.
    static PrefabInstance* InstantiateIntoLevel(Level& level, const std::filesystem::path& prefab_path);

    static void Register(const Level& level, PrefabInstance* instance);
    static void Unregister(PrefabInstance* instance);

    /// Instances registered for `level` (including ones whose root is currently
    /// deleted, e.g. held by undo).
    static std::vector<PrefabInstance*> GetInstances(const Level& level);

    /// Instances registered for `level` whose root GameObject is in the level's
    /// object table — exactly the set Level::save writes.
    static std::vector<PrefabInstance*> GetInstancesToSave(const Level& level);

    /// The registered instance whose subtree contains `game_object` (looked up
    /// through the corresponding-source map), or nullptr.
    static PrefabInstance* FindInstanceForGameObject(const Level& level, const GameObject* game_object);

    /// Rewrites every GameObject reference held by `instance` (root, instance side
    /// of the corresponding-source map, modification targets and object references)
    /// whose instance ID is a key of `from_to` to the mapped GameObject.
    static void RetargetGameObjects(PrefabInstance& instance, const std::unordered_map<int32_t, GameObject*>& from_to);
};
