#pragma once

#include "Runtime/BaseClasses/PPtr.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Resource/Prefab/PrefabAsset.h"

class GameObject;
class Transform;

// =====================================================================================
// PrefabRefComponent (Runtime) — Phase 3a: Nested Prefab support.
// -------------------------------------------------------------------------------------
// Marks a GameObject as a NESTED-PREFAB INSTALL POINT. When the parent prefab is
// instantiated through `PrefabUtility::Instantiate`, the post-load driver detects
// this component and:
//
//   1. Loads the referenced PrefabAsset (`m_ReferencedPrefab`).
//   2. Instantiates it via PrefabUtility::Instantiate (recursive — nested-of-nested
//      works automatically).
//   3. Reparents the instantiated subtree under the host GameObject's
//      Transform (worldPositionStays = false, so the host's local pose is
//      respected by the nested root).
//
// Mirrors Unity's Nested Prefab concept (Editor/Src/Prefabs/PrefabInstance.cpp's
// `m_SourcePrefab` mechanic), simplified for ZEngine: the host GameObject acts as
// the "PrefabInstance" carrier, and its overrides on the nested subtree are handled
// by the Editor-side PrefabInstance bookkeeping (see PrefabUtilityEditor).
//
// Authored serialised fields:
//   * m_ReferencedPrefab — PPtr<PrefabAsset> to the prefab to expand here.
//
// IMPORTANT: a PrefabRefComponent should be the ONLY component on its host
// GameObject (besides its Transform). Mixing other components on the host
// works at runtime, but the editor's nested-prefab GUI assumes the install point
// is "thin".
// =====================================================================================
class PrefabRefComponent : public Component
{
    REGISTER_CLASS(PrefabRefComponent);
    DECLARE_OBJECT_SERIALIZE();

public:
    PrefabRefComponent() = default;

    void SetReferencedPrefab(PPtr<PrefabAsset> p) { m_ReferencedPrefab = p; }
    PPtr<PrefabAsset> GetReferencedPrefab() const { return m_ReferencedPrefab; }

    /// True after `Expand` has installed the nested subtree (so PrefabUtility's
    /// post-load driver doesn't double-expand on re-entry).
    bool IsExpanded() const { return m_Expanded; }
    void MarkExpanded() { m_Expanded = true; }

protected:
    PPtr<PrefabAsset> m_ReferencedPrefab;

    /// Editor-time-only marker — `PrefabUtility::Instantiate` flips this on after
    /// the first expansion to ensure idempotency. Persisted so saved scenes that
    /// already contain expanded nested subtrees don't get a second expansion when
    /// re-loaded.
    bool m_Expanded {false};
};
