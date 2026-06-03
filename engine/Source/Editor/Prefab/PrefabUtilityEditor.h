#pragma once

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/BaseClasses/PPtr.h"

#include <EASTL/string.h>
#include <cstdint>
#include <filesystem>
#include <vector>

class PrefabAsset;
class PrefabInstance;
class Object;

// =====================================================================================
// PrefabUtilityEditor — Editor-side counterpart of Runtime PrefabUtility.
// -------------------------------------------------------------------------------------
// Mirrors a subset of Unity's `PrefabUtility` static API. Lives in the Editor module
// because everything here mutates or queries PrefabInstance bookkeeping, which is
// itself editor-only.
//
//   * InstantiateAsPrefab(asset)                Phase 2a / 2b-4b
//       Creates a new instance subtree AND a paired PrefabInstance bookkeeping
//       object whose `corresponding_source_map` is seeded from the clone via the
//       structural EditorPrefabCloner (preserves Transform parent/children + the
//       GameObject.m_Components ImmediatePtr list, both of which a generic
//       round-trip would lose).
//
//   * RecordPropertyOverride(...)               Phase 2b-4c
//       Append-or-overwrite a PropertyModification into instance.m_Modifications.
//       Editor GUIs call this whenever the user edits a serialised field on a
//       cloned object.
//
//   * ApplyAll(instance), Revert(instance)      Phase 2b-4c
//       Walk `instance.GetModifications()` and either push the override into the
//       source PrefabAsset (Apply) or pull the source's value back into the
//       instance (Revert), then clear the override list.
//
// All methods are static; PrefabUtilityEditor is a namespace-style class.
// =====================================================================================
class PrefabUtilityEditor
{
public:
    // ---- Phase 2a / 2b-4b: instantiate-with-bookkeeping ----

    /// Loads `prefab_path` from disk, structurally clones the subtree,
    /// allocates a fresh PrefabInstance, links it to the source PrefabAsset,
    /// populates its corresponding-source map, and returns the new
    /// PrefabInstance. Returns nullptr on any failure (asset missing,
    /// instantiate failed, ...).
    static PrefabInstance* InstantiateAsPrefab(const std::filesystem::path& prefab_path);

    /// Convenience overload: skip the disk read when the asset is already loaded.
    static PrefabInstance* InstantiateAsPrefab(PrefabAsset* asset);

    // ---- Phase 2b-4c: override recording / Apply / Revert ----

    /// Record (or overwrite) a property override on `instance`. The override
    /// targets the instance-side object `target` (must be one of the cloned
    /// objects — its source counterpart is looked up through
    /// instance.m_CorrespondingSource). `path` is a PropertyPath like
    /// "m_LocalPosition.x" or "m_Components[2].m_Enabled". `value_bytes` is
    /// the host-endian raw bytes that should appear at that field's slot in
    /// the StreamedBinary stream.
    ///
    /// If a modification with the same (target, path) already exists, its
    /// value is replaced in place. Returns true on success, false if the path
    /// fails to locate (typed mismatch, missing field, etc.).
    static bool RecordPropertyOverride(PrefabInstance* instance,
                                       PPtr<Object> target,
                                       const eastl::string& path,
                                       const std::vector<uint8_t>& value_bytes);

    /// Object-reference variant of RecordPropertyOverride. Used when the field
    /// being overridden is itself a PPtr<>; `object_reference` provides the
    /// new InstanceID.
    static bool RecordPropertyOverride(PrefabInstance* instance,
                                       PPtr<Object> target,
                                       const eastl::string& path,
                                       PPtr<Object> object_reference);

    /// Capture the current value of `(target, path)` from the instance's live
    /// state into a fresh PropertyModification, then forward to
    /// RecordPropertyOverride(value_bytes). This is the API editor GUIs use
    /// after the user mutates a field directly on the instance.
    static bool RecordCurrentValueAsOverride(PrefabInstance* instance,
                                             PPtr<Object> target,
                                             const eastl::string& path);

    /// Push every override in `instance.GetModifications()` back onto the
    /// source PrefabAsset (modifies the source objects in memory). After a
    /// successful pass, the modification list is cleared.
    /// Returns the number of modifications that applied successfully (callers
    /// can detect partial failure by comparing against initial size).
    static int ApplyAll(PrefabInstance* instance);

    /// Pull every override's source-side value back into the instance,
    /// effectively un-doing every recorded mutation, then clear the list.
    static int Revert(PrefabInstance* instance);

    /// Re-read the source-side value of a single (target, path) pair and
    /// stamp it into the instance. Does NOT touch m_Modifications — useful
    /// when the editor's "Revert this property" gesture is triggered.
    static bool RevertSingleOverride(PrefabInstance* instance,
                                     PPtr<Object> target,
                                     const eastl::string& path);

    // ---- Phase 2b-4d: added / removed components ----

    /// Attach `component` (already constructed by the editor) to `host_go` on
    /// the instance side, AND record it in instance.m_AddedComponents so
    /// future Apply/Revert/save passes know it's an instance-only override.
    /// `host_go` must be one of the cloned GameObjects in the instance subtree
    /// (the function does not validate that — caller is responsible).
    static bool AddComponentOverride(PrefabInstance* instance,
                                     PPtr<GameObject> host_go,
                                     Component* component);

    /// Detach a previously-added component from `host_go` and drop it from
    /// instance.m_AddedComponents. Does NOT destroy the Component object
    /// (caller may want to keep it around for undo).
    static bool RemoveAddedComponent(PrefabInstance* instance,
                                     PPtr<GameObject> host_go,
                                     Component* component);

    /// Mark `source_component` as suppressed on this instance: the instance-
    /// side counterpart of `source_component` (looked up via the
    /// corresponding-source map) is detached from its owning GameObject, and
    /// `source_component` is appended to instance.m_RemovedComponents.
    /// `source_component` must be a PPtr into the source PrefabAsset.
    static bool SuppressSourceComponent(PrefabInstance* instance,
                                        PPtr<Component> source_component);

    /// Re-introduce a previously-suppressed source component into the
    /// instance: clone `source_component` afresh, attach it to the instance-
    /// side host GameObject (via the corresponding-source map), drop the
    /// PPtr from instance.m_RemovedComponents.
    static bool UnsuppressSourceComponent(PrefabInstance* instance,
                                          PPtr<Component> source_component);

    // ---- Phase 3c: Unpack ----

    /// "Unpack" the PrefabInstance: drop the prefab linkage but KEEP the cloned
    /// GameObject subtree in place. The instance becomes plain, scene-owned
    /// GameObjects with no override tracking, no source map, and no future
    /// auto-merging when the source PrefabAsset changes.
    ///
    /// Mirrors Unity's `PrefabUtility.UnpackPrefabInstance(go, action)`:
    ///   1. Clears all bookkeeping fields (modifications, corresponding_source,
    ///      added_components, removed_components, source_prefab,
    ///      root_game_object).
    ///   2. Returns the cloned root GameObject so the caller can continue
    ///      working with it as a regular subtree.
    ///   3. Caller is responsible for destroying the now-empty PrefabInstance
    ///      bookkeeping object (typically through ObjectManager).
    ///
    /// Returns the kept root GameObject on success, nullptr if `instance` is
    /// null or has no root.
    static GameObject* UnpackPrefabInstance(PrefabInstance* instance);

    // ---- Phase 3b: Variant instantiation ----

    /// Instantiate `variant` (which must have a non-null variant-base chain).
    /// Behaviour: walk the base chain to find the deepest non-variant
    /// PrefabAsset, structurally clone its subtree, then for each ancestor
    /// (top-down: base → ... → variant) apply that ancestor's variant
    /// modifications to the corresponding cloned objects. Finally allocate a
    /// fresh PrefabInstance whose `m_SourcePrefab` points at the topmost
    /// `variant` argument (NOT at the deepest base).
    ///
    /// This is the editor entry-point that matches Unity's
    /// `PrefabUtility.InstantiatePrefab(variant)` semantics for Variant assets.
    /// Non-variant assets are forwarded to InstantiateAsPrefab(asset).
    static PrefabInstance* InstantiateVariant(PrefabAsset* variant);

    // ---- Phase 3d: Merge (refresh instance from updated source) ----

    /// Refresh `instance` against changes that were made to its source
    /// PrefabAsset since instantiation. Three-layer stack:
    ///
    ///   layer 0:  source PrefabAsset (variant base, if any)
    ///   layer 1:  variant overrides applied on top of layer 0 (if source is
    ///             a Variant)
    ///   layer 2:  instance overrides (instance.m_Modifications)
    ///
    /// MergeFromSource:
    ///   * For every (instance_obj, source_obj) pair in corresponding_source,
    ///     re-snapshot the source's bytes, splice in any layer-1 / layer-2
    ///     overrides, and read the merged result back into `instance_obj`.
    ///   * Detects added/removed components on the source side and propagates
    ///     them (within the limits of the bookkeeping — components that the
    ///     instance has explicitly suppressed are NOT re-introduced).
    ///
    /// Returns the number of objects whose merged value was written.
    static int MergeFromSource(PrefabInstance* instance);
};
