#pragma once

#include "Runtime/BaseClasses/PPtr.h"

#include <unordered_map>
#include <vector>

class GameObject;
class Component;
class Object;
class TransformComponent;

// =====================================================================================
// EditorPrefabCloner — structural deep-clone of a prefab subtree, paired with a
// source-id ↔ clone-id map.
// -------------------------------------------------------------------------------------
// Phase 2b-4b deliverable. Replaces the "re-read from disk" clone strategy used by
// Runtime PrefabUtility::Instantiate when the editor needs both source AND clone
// trees alive at the same time (so PrefabInstance::m_CorrespondingSource can
// answer "given this instance object, what's its baseline state in the source?").
//
// Why a hand-rolled cloner instead of a generic round-trip:
//   - PPtr<T>::Transfer is a no-op in ZEngine, so a serialise+deserialise round
//     trip wipes every PPtr to InstanceID=0. A prefab's TransformComponent.m_Parent
//     and m_Children chain is entirely PPtrs — losing them would flatten the
//     hierarchy.
//   - ImmediatePtr<T>::Transfer only emits LocalSerializedObjectIdentifier metadata
//     without restoring the runtime pointer; the GameObject.m_Components pointers
//     would all come back as nullptr.
//
// Strategy:
//   1) Walk the source subtree (BFS over GameObjects via Transform.m_Children +
//      every GameObject's Component list) and allocate a fresh clone for every
//      Object encountered. Record `(source_id → clone_id)` and `(source_ptr →
//      clone_ptr)` in `id_map` / `ptr_map`.
//   2) For each (source, clone) pair, copy serialised scalar/string/array fields
//      with TransferUtility::WriteObjectToVector → ReadObjectFromVector — that
//      faithfully transports plain data without touching pointers.
//   3) Manually re-wire pointers using `ptr_map`:
//          - GameObject.m_Components ImmediatePtrs ← look up each component's
//            clone counterpart in ptr_map.
//          - TransformComponent.m_Parent / m_Children PPtrs ← same lookup.
//          - Component.m_ParentObject raw pointer ← reset via postLoadResource
//            during PrefabPostLoadDriver's pass.
//
// This module is intentionally inside the Editor module because it depends on
// PrefabInstance bookkeeping conventions; Runtime code should keep using the
// existing from-disk re-read path which is sufficient for non-edited Instantiate.
// =====================================================================================

namespace prefab_editor
{

    /// Output of CloneSubtree. `id_map[i]` and `ptr_map[i]` describe paired source/clone
    /// objects in DFS-on-Transform-children visit order; the editor stores the same
    /// pairs into PrefabInstance::m_CorrespondingSource.
    struct CloneResult
    {
        /// Top-level clone of the subtree's root GameObject. nullptr on failure.
        GameObject* clone_root {nullptr};

        /// Parallel arrays — sources[i] paired with clones[i] for every Object reached.
        /// Both GameObjects and Components are included.
        std::vector<Object*> sources;
        std::vector<Object*> clones;

        /// Quick source*→clone* lookup; populated alongside the parallel arrays.
        std::unordered_map<Object*, Object*> ptr_map;

        bool valid() const { return clone_root != nullptr; }
    };

    /// Deep-clones `source_root` and the subtree reachable through its Components +
    /// Transform.m_Children chain. On success returns a CloneResult whose `clone_root`
    /// is a fresh, fully-allocated GameObject independent of `source_root`.
    ///
    /// The clone tree's Components have NOT yet had PostLoadResource() invoked — the
    /// caller is expected to drive that pass (see PrefabPostLoadDriver in
    /// Runtime/Resource/Prefab/PrefabUtility.cpp) so that Component.m_ParentObject
    /// gets re-bound to the clone-side GameObjects.
    CloneResult CloneSubtree(GameObject* source_root);

}  // namespace prefab_editor
