#pragma once

#include "Runtime/BaseClasses/GameObject.h"

#include <filesystem>
#include <unordered_map>
#include <vector>

class PrefabAsset;
class Transform;
class Level;
class Object;

// =====================================================================================
// PrefabUtility (Runtime)
// -------------------------------------------------------------------------------------
// Public entry points for instantiating prefab assets at runtime. Mirrors the runtime
// half of Unity's `PrefabUtility` (the editor-side Apply/Revert/Override APIs live in
// the Editor module — see engine/Source/Editor/Prefab/).
//
// Instantiation contract (matches Unity Object.Instantiate(Prefab) semantics):
//   * Each call returns a fully independent GameObject tree with fresh InstanceID and
//     GObjectID for every Object in the cloned subtree.
//   * The result has NO link back to the source PrefabAsset — modifications to the
//     source after instantiation will not propagate. Use the Editor-side
//     `PrefabInstance` if such a link is required.
//   * `postLoadResource` is invoked on every cloned Component once the tree is built,
//     so Component-side caching (e.g. m_ParentObject, transform double-buffer) is
//     ready before the caller sees the GameObject.
//
// Implementation (2026-10-26): the prefab file's graph is loaded once and cached
//   (LoadPrefabAsset) -- it lives under path-derived instance IDs, so a second read
//   would collide with the first. Every Instantiate deep-clones that cached template
//   (prefab_clone::CloneSubtree, Runtime/Resource/Prefab/PrefabSubtreeCloner.h); the
//   template itself is never handed out. (The original strategy, "re-read the file
//   for every call", stopped producing independent objects once reads were cached.)
// =====================================================================================
class PrefabUtility
{
public:
    /// Instantiate the prefab and return its new root GameObject.
    /// The returned GO is freshly allocated and not yet attached to any Level — call
    /// `Level::adoptInstantiatedRoot()` (or equivalent) to register it.
    ///
    /// `invoke_post_load = false` is for the **definition-expansion** path only
    /// (`GameObject::ExpandDefinition`): there the returned GO is used purely as a
    /// bag of Components to merge into another GameObject, and it is then dropped
    /// on the floor. Running PostLoadResource on it would allocate GPU / id
    /// resources for an object that is never registered anywhere and never ticks —
    /// a particle emitter in the prefab leaks a whole emitter slot per scene object
    /// that references it. The caller must post-load the merged components itself
    /// with the correct parent (which `GameObject::load()` does).
    /// Nested-prefab expansion and the child DFS still run in both modes.
    static GameObject* Instantiate(PrefabAsset* asset, bool invoke_post_load = true);

    /// Convenience wrapper: load the prefab from disk, then Instantiate once.
    static GameObject* InstantiateFromPath(const std::filesystem::path& prefab_path,
                                           bool                        invoke_post_load = true);

    /// Load a prefab file as its WHOLE object graph and return its PrefabAsset header,
    /// with `GetRootGameObject()` resolved and every GameObject's runtime component
    /// list rebuilt. `out_gameobjects` (optional) receives every GameObject of the
    /// prefab -- what PrefabPostLoadDriver needs to resolve child Transforms at any depth.
    ///
    /// Why not `AssetManager::ReadObject<PrefabAsset>`: that reads ONE object (fileID 1).
    /// The header's m_RootGameObject is an in-file reference to fileID 2, which a
    /// single-object read never produces, so the root always came back null -- binary
    /// prefabs could not be instantiated at all, and the editor's
    /// InstantiateAsPrefab(path) failed for both formats.
    ///
    /// Both formats are registered under path-derived instance IDs, so a second read
    /// would re-produce the same IDs; the file is therefore read once and the cached
    /// graph is returned after that. The result is the TEMPLATE -- do not put it in a
    /// scene; Instantiate / InstantiateFromPath clone it.
    ///
    /// A **Variant** file legitimately has no root GameObject: it is a base PPtr
    /// plus overrides, and the tree lives in its base. Such a file loads fine and
    /// yields an empty `out_gameobjects`; only Instantiate (which walks the chain)
    /// can do anything useful with it.
    static PrefabAsset* LoadPrefabAsset(const std::filesystem::path& prefab_path,
                                        std::vector<GameObject*>*    out_gameobjects = nullptr);

    // ---- Prefab Variant (model A: true inheritance) ----
    //
    // A Variant owns NO object tree: it is `m_variant_base` plus the
    // PropertyModifications that this link changes on top of the base. Both halves
    // of that live on PrefabAsset (NOT PrefabInstance, which is kTypeIsEditorOnly
    // and would never reach the player).

    /// Walk `asset`'s variant base chain. On success `out_chain` is ordered
    /// deepest-base-first and ends with `asset` itself — a plain prefab yields a
    /// one-element chain. Returns false on a broken chain (a link marked variant
    /// whose base PPtr is null) or a cycle (>64 links).
    static bool ResolveVariantChain(PrefabAsset* asset, std::vector<PrefabAsset*>& out_chain);

    /// Apply every link's variant overrides onto an already-built clone.
    ///
    /// `source_to_clone` maps an object in the deepest base's graph -> its
    /// counterpart in the cloned tree (for `Instantiate` that is
    /// `CloneResult::ptr_map`; the editor passes its corresponding-source map
    /// instead). Links are applied top-down, so a nearer link wins over a farther
    /// one on the same field.
    ///
    /// An override whose target is missing from `source_to_clone` (the base was
    /// edited and no longer has that object) is skipped with a warning rather than
    /// failing the whole instantiation. Returns how many were applied.
    static int ApplyVariantOverrides(const std::vector<PrefabAsset*>&        chain,
                                     const std::unordered_map<Object*, Object*>& source_to_clone);

    /// Create an in-memory Variant of `base`: a PrefabAsset with NO object tree of
    /// its own, carrying only `m_variant_base`. The caller owns the result and is
    /// expected to fill `GetVariantOverrides()` before persisting it with
    /// SaveVariantAsset().
    ///
    /// ⚠️ Tear it down with `ObjectManager::Destroy` — Produce() is `new`, and
    /// pairing `new` with MemoryManager::DestroyObject crashes in mi_usable_size
    /// on Apple (see the longer note in SaveAsPrefabAsset).
    ///
    /// Returns nullptr if `base` is null or the header cannot be allocated.
    static PrefabAsset* CreateVariantAsset(PrefabAsset* base, const eastl::string& variant_name);

    /// Write `variant` to `variant_path`. The file contains exactly one object —
    /// the PrefabAsset header — because a Variant owns no tree; the base reference
    /// is a cross-file PPtr and the overrides ride along in `m_variant_overrides`.
    ///
    /// `variant_path` must end in .prefab (YAML) or .zasset (binary SerializedFile),
    /// matching SaveAsPrefabAsset's split.
    static bool SaveVariantAsset(PrefabAsset* variant, const std::filesystem::path& variant_path);

    /// Re-parent the freshly instantiated root under `parent` (Transform-based).
    /// `worldPositionStays` follows Unity semantics (see Transform::SetParent).
    static void SetInstantiatedRootParent(GameObject* instantiated_root, Transform* parent, bool worldPositionStays = true);

    // -----------------------------------------------------------------------------
    // Authoring (Hierarchy → Project drag-and-drop, "Save as Prefab")
    // -----------------------------------------------------------------------------
    /// Take a live GameObject subtree and persist it as a `.zasset` Prefab on disk.
    ///
    /// Behaviour mirrors Unity's "drag a GameObject from Hierarchy into Project":
    ///   * The provided `root` and every GameObject reachable through its
    ///     Transform.m_Children chain (plus all their Components) is
    ///     packaged into a fresh PrefabAsset and written to `prefab_path` via the
    ///     binary SerializedFile pipeline (`AssetManager::WriteObjectsToDiskThreadSafe`).
    ///   * The PrefabAsset is the first object in the file (fileID=1); the root
    ///     GameObject is fileID=2; every other GO/Component is assigned by DFS order.
    ///     Internal cross-object references (ImmediatePtr / PPtr) inside the subtree
    ///     are therefore self-contained — exactly the contract Phase 1 expects on
    ///     the load side (see `PrefabPostLoadDriver` in PrefabUtility.cpp).
    ///   * The live scene tree is NOT mutated. The caller's `root` continues to
    ///     reside in its original Level; this function only reads the subtree.
    ///   * Returns true on a successful disk write (file present + writer reported
    ///     no error). Returns false on any precondition failure (null root, no
    ///     Transform on root, AssetManager rejected the path, …).
    ///
    /// `prefab_path` should already include the `.zasset` extension. The caller is
    /// responsible for picking a non-colliding filename — the Editor wrapper in
    /// Content Browser does that before calling in.
    static bool SaveAsPrefabAsset(GameObject* root, const std::filesystem::path& prefab_path);

private:
    PrefabUtility() = delete;
    PrefabUtility(const PrefabUtility&) = delete;
    PrefabUtility& operator=(const PrefabUtility&) = delete;
};
