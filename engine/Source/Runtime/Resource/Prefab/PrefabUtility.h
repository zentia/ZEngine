#pragma once

#include "Runtime/BaseClasses/GameObject.h"

#include <filesystem>

class PrefabAsset;
class Transform;
class Level;

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
// Phase 1 implementation strategy:
//   The cleanest, lowest-risk way to deep-clone the Object tree is to round-trip it
//   through the existing binary serialization stack. AssetManager::ReadObject<> reads
//   the .zasset, allocating every contained Object via ObjectManager — exactly what
//   we want. We therefore re-load the prefab from disk for every Instantiate call and
//   reparent the freshly read root. A future phase can replace this with an in-memory
//   buffer round-trip (BufferedCacheWriter -> CacheReader) for performance, without
//   changing the public API surface here.
// =====================================================================================
class PrefabUtility
{
public:
    /// Instantiate the prefab and return its new root GameObject.
    /// The returned GO is freshly allocated and not yet attached to any Level — call
    /// `Level::adoptInstantiatedRoot()` (or equivalent) to register it.
    static GameObject* Instantiate(PrefabAsset* asset);

    /// Convenience wrapper: load the prefab from disk, then Instantiate once.
    static GameObject* InstantiateFromPath(const std::filesystem::path& prefab_path);

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
