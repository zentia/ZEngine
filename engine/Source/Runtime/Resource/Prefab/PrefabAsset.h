#pragma once

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/BaseClasses/ImmediatePtr.h"
#include "Runtime/BaseClasses/Object.h"
#include "Runtime/BaseClasses/PPtr.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"
#include "Runtime/Resource/Prefab/PropertyModification.h"
#include "PrefabAsset.generated.h"

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
ZCLASS()
class PrefabAsset : public Object
{
    ZGENERATED_BODY(PrefabAsset)

public:
    PrefabAsset() = default;

    // ---- Authoring/inspection accessors ----
    GameObject* GetRootGameObject() const { return m_RootGameObject; }
    void SetRootGameObject(GameObject* root) { m_RootGameObject = root; }

    // ---- Variant chain (Phase 3 will populate; safe to leave empty in Phase 1/2) ----
    PrefabAsset* GetVariantBase() const { return m_VariantBase; }
    void SetVariantBase(PrefabAsset* base) { m_VariantBase = base; }

    bool IsVariant() const { return !m_VariantBase.IsNull(); }

    // ---- Variant overrides (model A) ----
    //
    // A Variant owns NO object tree of its own: it is a base pointer plus the list
    // of PropertyModifications that this variant (and each intermediate variant in
    // the chain) applies on top of the deepest non-variant base.
    //
    // `target` in each entry is a PPtr naming an object **in the base's graph**
    // (a file-local fileID in the base's .prefab / .zasset). Every link in a
    // variant chain therefore targets the SAME deepest base's objects, which is
    // what lets a chain of any length be applied to one cloned base tree:
    //   base objects --clone--> clone objects --apply--> base overrides
    //                                                --> mid overrides
    //                                                --> variant overrides
    //
    // Why these live on the asset (and not on the editor-only PrefabInstance,
    // which is what doc/PrefabSystem_Design.md §9.3 originally settled on):
    // PrefabInstance is `kTypeIsEditorOnly`, so a variant's overrides could never
    // reach the runtime — `GameObject.Instantiate(variantPath)` on device would
    // silently produce a plain copy of the base. Storing them here is what makes
    // "Variant" mean anything outside the editor.
    std::vector<PropertyModification>& GetVariantOverrides() { return m_VariantOverrides; }
    const std::vector<PropertyModification>& GetVariantOverrides() const { return m_VariantOverrides; }
    void AddVariantOverride(const PropertyModification& mod) { m_VariantOverrides.push_back(mod); }
    void ClearVariantOverrides() { m_VariantOverrides.clear(); }

    // ---- Schema versioning (so future format breaks are detectable) ----
    uint32_t GetSchemaVersion() const { return m_SchemaVersion; }

    /// Bumped 1 -> 2 when `m_variant_overrides` was appended. Older files keep
    /// reporting 1; the field is appended last so readers that don't know it
    /// still resolve every earlier field by name.
    static constexpr uint32_t kCurrentSchemaVersion = 2u;

protected:
    // Serialized order: m_schema_version, m_root_game_object, m_variant_base,
    // m_variant_overrides. Appending is what keeps old readers working.
    ZPROPERTY(Name="m_schema_version")
    uint32_t m_SchemaVersion {kCurrentSchemaVersion};

    /// Root GameObject — the entry point of the prefab tree. Other GOs/Components are
    /// reachable through its Transform.m_Children chain and its m_Components list.
    ZPROPERTY(Name="m_root_game_object")
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
    ZPROPERTY(Name="m_variant_base")
    PPtr<PrefabAsset> m_VariantBase;

    /// What this Variant changes relative to its base chain. Empty for a plain
    /// prefab. See the accessors above for the model.
    ZPROPERTY(Name="m_variant_overrides")
    std::vector<PropertyModification> m_VariantOverrides;
};
