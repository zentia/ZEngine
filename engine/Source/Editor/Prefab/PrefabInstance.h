#pragma once

#include "Editor/Prefab/PropertyModification.h"
#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/BaseClasses/Object.h"
#include "Runtime/BaseClasses/PPtr.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Resource/Prefab/PrefabAsset.h"

#include <vector>

// Note: PrefabAsset is included as a complete type (not forward-declared) because
// the inline accessors `SetSourcePrefab` / `GetSourcePrefab` instantiate
// `PPtr<PrefabAsset>::GetTypeString`, which requires `PrefabAsset::GetPPtrTypeString`
// — that symbol is injected by the `REGISTER_CLASS` macro and only visible once the
// full PrefabAsset declaration has been seen.

// =====================================================================================
// PrefabInstance — Editor-only override container for a placed prefab.
// -------------------------------------------------------------------------------------
// Conceptually mirrors Unity's `Editor/Src/Prefabs/PrefabInstance.h`:
//
//   * Lives only in the Editor module (kTypeIsEditorOnly trait).
//   * Owns no GameObject/Component objects directly — they live in the same
//     SerializedFile (scene or parent prefab) as the PrefabInstance itself.
//   * Holds: source PrefabAsset, root GameObject of the placed instance, the override
//     list (PropertyModifications), the corresponding-source map (instance-side
//     object ↔ source-side object), and the added/removed-component lists.
//
// Phase 2a contract:
//   * Skeleton + serialisation only. No Apply/Revert/Merge yet — those are Phase 2b.
//   * PrefabUtilityEditor::InstantiateAsPrefab(asset) creates the instance and seeds
//     the `corresponding_source_map` from the freshly-cloned subtree (paired with
//     its source PrefabAsset).
//
// IMPORTANT — relationship with Runtime PrefabAsset:
//   PrefabInstance does NOT replace PrefabUtility::Instantiate(asset). That call
//   produces a "raw" GameObject instance (no override tracking, no source map —
//   essentially a Variant-with-zero-overrides). InstantiateAsPrefab is the editor-
//   facing version: it does the same clone + additionally registers the
//   PrefabInstance bookkeeping object.
// =====================================================================================
class PrefabInstance : public Object
{
    REGISTER_CLASS(PrefabInstance);
    REGISTER_CLASS_TRAITS(kTypeIsEditorOnly);
    DECLARE_OBJECT_SERIALIZE();

public:
    PrefabInstance() = default;

    // ---- Source prefab ----
    void SetSourcePrefab(PPtr<PrefabAsset> prefab) { m_SourcePrefab = prefab; }
    PPtr<PrefabAsset> GetSourcePrefab() const { return m_SourcePrefab; }

    // ---- Root GameObject ----
    void SetRootGameObject(PPtr<GameObject> root) { m_RootGameObject = root; }
    PPtr<GameObject> GetRootGameObject() const { return m_RootGameObject; }

    // ---- Property overrides ----
    std::vector<PropertyModification>& GetModifications() { return m_Modifications; }
    const std::vector<PropertyModification>& GetModifications() const { return m_Modifications; }
    void AddModification(const PropertyModification& mod) { m_Modifications.push_back(mod); }
    void ClearModifications() { m_Modifications.clear(); }

    // ---- Corresponding-source map (instance object ↔ source object) ----
    //
    // One entry per cloned object (GameObject AND Component). The map lets the apply
    // pipeline answer "given this instance object, what's its baseline state in the
    // source PrefabAsset?" — required for both Apply ("write the difference back to
    // source") and Revert ("re-pull the source's value into the instance").
    //
    // Stored as parallel arrays rather than a hash map so it round-trips through the
    // Transfer macros without a custom serialiser. Lookup is O(N) but N is bounded by
    // the prefab's object count, which is small in practice.
    struct CorrespondingSourceEntry
    {
        DECLARE_SERIALIZE(CorrespondingSourceEntry)

        PPtr<Object> instance_object;
        PPtr<Object> source_object;
    };

    std::vector<CorrespondingSourceEntry>& GetCorrespondingSourceMap() { return m_CorrespondingSource; }
    const std::vector<CorrespondingSourceEntry>& GetCorrespondingSourceMap() const { return m_CorrespondingSource; }

    /// Linear lookup: returns the source object paired with `instance_object`, or a
    /// null PPtr if the mapping is missing (which usually means the object was added
    /// in this instance and isn't part of the source — see `m_AddedComponents`).
    PPtr<Object> FindSourceForInstance(PPtr<Object> instance_object) const;

    /// Linear lookup: returns the instance object paired with `source_object`, or a
    /// null PPtr if the mapping is missing (which means the source object was
    /// removed in this instance — see `m_RemovedComponents`).
    PPtr<Object> FindInstanceForSource(PPtr<Object> source_object) const;

    // ---- Added components (exist on the instance, not on source) ----
    std::vector<PPtr<Component>>& GetAddedComponents() { return m_AddedComponents; }
    const std::vector<PPtr<Component>>& GetAddedComponents() const { return m_AddedComponents; }

    // ---- Removed components (exist on source, suppressed on the instance) ----
    std::vector<PPtr<Component>>& GetRemovedComponents() { return m_RemovedComponents; }
    const std::vector<PPtr<Component>>& GetRemovedComponents() const { return m_RemovedComponents; }

protected:
    /// The PrefabAsset this instance was placed from.
    PPtr<PrefabAsset> m_SourcePrefab;

    /// Topmost GameObject of the instantiated subtree.
    PPtr<GameObject> m_RootGameObject;

    /// All overrides this instance applies on top of `m_SourcePrefab`.
    std::vector<PropertyModification> m_Modifications;

    /// Pairs every cloned instance object with its source counterpart.
    std::vector<CorrespondingSourceEntry> m_CorrespondingSource;

    /// Components on the instance that have no source counterpart (newly added in
    /// this scene/instance). Stored as PPtrs rather than ImmediatePtrs because they
    /// ARE part of the cloned tree and live in the same SerializedFile — but we
    /// don't *own* them, the host GameObject does.
    std::vector<PPtr<Component>> m_AddedComponents;

    /// Source-side components that this instance suppresses. Cross-file PPtrs
    /// pointing into the source PrefabAsset.
    std::vector<PPtr<Component>> m_RemovedComponents;
};

template<class TransferFunction>
inline void PrefabInstance::CorrespondingSourceEntry::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(instance_object, "instance_object");
    transfer.Transfer(source_object, "source_object");
}
