#pragma once

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/BaseClasses/ImmediatePtr.h"
#include "Runtime/BaseClasses/Object.h"
#include "Runtime/BaseClasses/PPtr.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"

// =====================================================================================
// PrefabAsset
// -------------------------------------------------------------------------------------
// Runtime-side, persistent on-disk representation of a prefab. Produced by saving an
// authored game-object subtree as a `.zasset` file (binary SerializedFile, the unified
// asset container shared by every ZEngine asset type — same convention as UE's `.uasset`).
//
// A PrefabAsset owns the root GameObject and (transitively) all Components & nested
// PrefabInstance objects belonging to the prefab. Internal references are stored as
// ImmediatePtr<> (file-local fileID + pathID), so they round-trip through the binary
// `.zasset` writer/reader without external dependencies.
//
// External references (e.g. a MeshRenderer pointing to a Material from another asset)
// continue to use PPtr<> with cross-file GUIDs, identical to all other ZEngine assets.
//
// This class is intentionally small: structural editing, override tracking, Apply/Revert
// and the editor-only PrefabInstance live in the Editor module
// (engine/Source/Editor/Prefab/), keeping the Runtime payload minimal.
//
// Phase 1 deliberately omits a flat object list. Consumers that need to enumerate every
// reachable GameObject/Component walk the root's Transform child chain — that walk is
// already required by InvokePostLoadResource() and lives in PrefabUtility.cpp.
// =====================================================================================
class PrefabAsset : public Object
{
    REGISTER_CLASS(PrefabAsset);
    DECLARE_OBJECT_SERIALIZE();

public:
    PrefabAsset() = default;

    // ---- Authoring/inspection accessors ----
    GameObject* GetRootGameObject() const { return m_RootGameObject; }
    void SetRootGameObject(GameObject* root) { m_RootGameObject = root; }

    // ---- Variant chain (Phase 3 will populate; safe to leave empty in Phase 1/2) ----
    PrefabAsset* GetVariantBase() const { return m_VariantBase; }
    void SetVariantBase(PrefabAsset* base) { m_VariantBase = base; }

    bool IsVariant() const { return !m_VariantBase.IsNull(); }

    // ---- Schema versioning (so future format breaks are detectable) ----
    uint32_t GetSchemaVersion() const { return m_SchemaVersion; }

    static constexpr uint32_t kCurrentSchemaVersion = 1u;

protected:
    /// Root GameObject — the entry point of the prefab tree. Other GOs/Components are
    /// reachable through its Transform.m_Children chain and its m_Components list.
    ImmediatePtr<GameObject> m_RootGameObject;

    // NOTE: ZEngine intentionally has no per-asset stable GUID stored on the asset
    // class itself, mirroring UE's `.uasset` model:
    //   * UE does not use side-car `.meta` files. Cross-package references go through
    //     `FSoftObjectPath` (package path + object name), not GUIDs.
    //   * UE's `FPackageFileSummary::PackageGuid` is purely a content fingerprint for
    //     cooker/source-control change detection — it is NOT used to resolve refs.
    // ZEngine follows the same principle: cross-`.zasset` references use PPtr, which
    // is (file path, localIdentifierInFile). AssetManager resolves them via
    // `GetInstanceIDFromPathAndFileID()` — no GUID lookup involved. If a content
    // fingerprint is later required for incremental cooking, it should live in the
    // SerializedFile header, not on individual Object subclasses.

    /// Non-null when this prefab is a Variant. Cross-file PPtr — the referenced
    /// PrefabAsset may live in a different .zasset file.
    PPtr<PrefabAsset> m_VariantBase;

    uint32_t m_SchemaVersion {kCurrentSchemaVersion};
};
