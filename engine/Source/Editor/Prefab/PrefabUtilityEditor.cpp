#include "Editor/Prefab/PrefabUtilityEditor.h"

#include "Runtime/Resource/Prefab/PrefabSubtreeCloner.h"
#include "Editor/Prefab/PrefabInstance.h"
#include "Runtime/Resource/Prefab/PropertyPathResolver.h"
#include "Runtime/Resource/Prefab/PropertyValueLocator.h"
#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/BaseClasses/Object.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Serialize/TransferUtility.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/Prefab/PrefabAsset.h"
#include "Runtime/Resource/Prefab/PrefabUtility.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include "Runtime/BaseClasses/Cast.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Editor/Prefab/PrefabOverrideModel.h"

namespace
{

    /// Resolve `target` to its cloned-instance-side Object* by inspecting the
    /// PrefabInstance's corresponding-source map. The override pipeline always
    /// records `target` as the INSTANCE-side PPtr; if the editor passed a source-
    /// side PPtr by mistake we silently invert the lookup so the caller still
    /// gets the instance pointer they need.
    Object* ResolveInstanceObject(PrefabInstance* instance, PPtr<Object> target)
    {
        if (instance == nullptr || target.IsNull())
            return nullptr;
        Object* candidate = static_cast<Object*>(target);
        if (candidate == nullptr)
            return nullptr;

        // If `target` already points at an instance-side object (it appears as
        // instance_object in the corresponding-source map), use it directly.
        if (!instance->FindSourceForInstance(target).IsNull())
            return candidate;

        // Otherwise treat it as source-side and look up the paired instance.
        PPtr<Object> instance_pptr = instance->FindInstanceForSource(target);
        return instance_pptr.IsNull() ? candidate : static_cast<Object*>(instance_pptr);
    }

    /// Snapshot an object as `(type_tree, bytes)` in one pass — the StreamedBinary
    /// bytes are what apply/revert splice into, the TypeTree describes how to
    /// navigate them.
    /// Overwrite `obj`'s serialized fields in place, then tell it so. Without the
    /// notification a component that keeps live state outside its serialized fields
    /// never sees the change -- Transform reads its TRS from the hierarchy buffer, so
    /// Apply / Revert / Merge on a Transform were invisible. Same contract the Undo
    /// system and the Inspector use (Component::OnSerializedFieldsUpdated).
    bool ReadBackInto(Object& obj, std::vector<uint8_t>& bytes)
    {
        if (!TransferUtility::ReadObjectFromVector(obj, bytes))
            return false;
        if (auto* component = Cast<Component>(&obj))
            component->OnSerializedFieldsUpdated();
        return true;
    }

    bool SnapshotObject(Object& obj, prefab::SerialisedObject& out_tree, std::vector<uint8_t>& out_bytes)
    {
        if (!prefab::SerialiseObject(&obj, out_tree))
            return false;
        out_bytes.clear();
        TransferUtility::WriteObjectToVector(obj, out_bytes);
        return true;
    }

    /// Apply a single PropertyModification by copying the relevant bytes from
    /// `donor_bytes` into `recipient`'s serialised stream and reading the result
    /// back into `recipient`. Used by both Apply (donor=instance, recipient=source)
    /// and Revert (donor=source, recipient=instance).
    bool SpliceFieldValueInto(Object& recipient,
                              const eastl::string& path,
                              const std::vector<uint8_t>* raw_bytes_override,  // explicit override (RecordPropertyOverride.value_bytes)
                              const std::vector<uint8_t>* donor_bytes,         // OR pull bytes from donor at same path
                              const prefab::SerialisedObject* donor_tree)
    {
        // 1) Snapshot the recipient — we need its current bytes + type tree to
        //    know where to splice.
        prefab::SerialisedObject recipient_tree;
        std::vector<uint8_t> recipient_bytes;
        if (!SnapshotObject(recipient, recipient_tree, recipient_bytes))
        {
            LOG_WARNING(ZPrefab, "SpliceFieldValueInto: snapshot of recipient failed");
            return false;
        }

        // 2) Tokenise the path once.
        std::vector<prefab::PathToken> tokens;
        if (!prefab::TokenizePropertyPath(path, tokens))
        {
            LOG_WARNING(ZPrefab, "SpliceFieldValueInto: malformed property_path '{}'", path.c_str());
            return false;
        }

        // 3) Locate the field on the recipient's stream.
        prefab::PropertyLocation recipient_loc;
        if (!prefab::LocatePropertyInStream(recipient_tree.type_tree, recipient_bytes, tokens, recipient_loc))
        {
            LOG_WARNING(ZPrefab, "SpliceFieldValueInto: path '{}' not found on recipient — {}", path.c_str(),
                        prefab::ExplainPathFailure(recipient_tree.type_tree, tokens).c_str());
            return false;
        }
        if (!recipient_loc.valid())
            return false;

        // 4) Determine the source bytes to splice in. Two modes:
        //    a) Explicit raw bytes (RecordPropertyOverride supplied them directly).
        //    b) Donor-pull: re-locate the same path on `donor_bytes` using
        //       `donor_tree` and copy the slice.
        const uint8_t* src_ptr = nullptr;
        size_t src_size = 0;
        std::vector<uint8_t> donor_slice;  // backing store for case (b)

        if (raw_bytes_override != nullptr)
        {
            if (raw_bytes_override->size() != static_cast<size_t>(recipient_loc.byte_size))
            {
                LOG_WARNING(ZPrefab,
                            "SpliceFieldValueInto: raw_bytes_override size {} != field size {} for path '{}'",
                            static_cast<int>(raw_bytes_override->size()),
                            static_cast<int>(recipient_loc.byte_size),
                            path.c_str());
                return false;
            }
            src_ptr = raw_bytes_override->data();
            src_size = raw_bytes_override->size();
        }
        else if (donor_bytes != nullptr && donor_tree != nullptr)
        {
            prefab::PropertyLocation donor_loc;
            if (!prefab::LocatePropertyInStream(donor_tree->type_tree, *donor_bytes, tokens, donor_loc))
            {
                LOG_WARNING(ZPrefab, "SpliceFieldValueInto: path '{}' not found on donor — {}", path.c_str(),
                            prefab::ExplainPathFailure(donor_tree->type_tree, tokens).c_str());
                return false;
            }
            if (donor_loc.byte_size != recipient_loc.byte_size)
            {
                LOG_WARNING(ZPrefab,
                            "SpliceFieldValueInto: donor field size {} != recipient field size {} for path '{}' (type changed?)",
                            static_cast<int>(donor_loc.byte_size),
                            static_cast<int>(recipient_loc.byte_size),
                            path.c_str());
                return false;
            }
            donor_slice.assign(
                donor_bytes->begin() + donor_loc.byte_offset,
                donor_bytes->begin() + donor_loc.byte_offset + donor_loc.byte_size);
            src_ptr = donor_slice.data();
            src_size = donor_slice.size();
        }
        else
        {
            LOG_WARNING(ZPrefab, "SpliceFieldValueInto: neither raw bytes nor donor supplied");
            return false;
        }

        // 5) Splice and read the bytes back into the recipient object so the
        //    in-memory object reflects the change.
        if (!prefab::OverwriteScalarBytes(recipient_bytes, recipient_loc, src_ptr, src_size))
        {
            LOG_WARNING(ZPrefab, "SpliceFieldValueInto: OverwriteScalarBytes failed for path '{}'", path.c_str());
            return false;
        }
        if (!ReadBackInto(recipient, recipient_bytes))
        {
            LOG_WARNING(ZPrefab, "SpliceFieldValueInto: ReadObjectFromVector failed for path '{}'", path.c_str());
            return false;
        }
        return true;
    }

    /// Encode a PPtr<Object>'s InstanceID as 4 host-endian bytes — the layout
    /// StreamedBinary uses for PPtr leaves. (PPtr<T>::Transfer is a no-op, but the
    /// ZEngine convention reflected in the TypeTree is still that a PPtr occupies
    /// a single int32_t slot for its InstanceID. This is the same convention
    /// SafeBinaryRead uses to walk past PPtr leaves.)
    std::vector<uint8_t> EncodePPtrAsBytes(PPtr<Object> ref)
    {
        int32_t id = ref.GetInstanceID();
        std::vector<uint8_t> out(sizeof(int32_t));
        std::memcpy(out.data(), &id, sizeof(int32_t));
        return out;
    }

    /// Snapshot the value an instance object should REVERT / MERGE to, plus its type
    /// tree, WITHOUT mutating `source_obj`.
    ///
    /// `source_obj` is a base template object — shared by every instance of that base —
    /// so layering the Variant's overrides onto it in place would corrupt all of them.
    /// `PrefabUtility::SnapshotWithOverrides` layers them onto the BYTES instead.
    ///
    /// For a plain prefab the variant layer is empty and this is just a snapshot.
    ///
    /// The layer is the WHOLE chain's overrides, deepest first (nearer wins). Until
    /// 2026-10-27 this took only the leaf variant's list, so on base <- mid <- leaf a
    /// field mid set and leaf did not reverted to the BASE value instead of mid's.
    void VariantLayerOf(const PrefabInstance* instance, std::vector<PropertyModification>& out_layer)
    {
        out_layer.clear();
        if (instance == nullptr)
            return;
        PrefabAsset* source = static_cast<PrefabAsset*>(instance->GetSourcePrefab());
        if (source == nullptr || !source->IsVariant())
            return;
        std::vector<PrefabAsset*> chain;
        if (PrefabUtility::ResolveVariantChain(source, chain))
            PrefabUtility::CollectVariantLayerOverrides(chain, out_layer);
    }

    bool SnapshotEffectiveSource(Object& source_obj, const PrefabInstance* instance,
                                 prefab::SerialisedObject& out_tree, std::vector<uint8_t>& out_bytes)
    {
        std::vector<PropertyModification> layer;
        VariantLayerOf(instance, layer);
        return PrefabUtility::SnapshotWithOverrides(source_obj, layer, out_tree, out_bytes);
    }

    /// Drop the corresponding-source entry whose instance side is `instance_object`.
    void ErasePairForInstanceObject(PrefabInstance* instance, const Object* instance_object)
    {
        if (instance == nullptr || instance_object == nullptr)
            return;
        const int32_t id = instance_object->GetInstanceID();
        auto& map = instance->GetCorrespondingSourceMap();
        map.erase(std::remove_if(map.begin(), map.end(),
                                 [id](const PrefabInstance::CorrespondingSourceEntry& e) {
                                     return e.instance_object.GetInstanceID() == id;
                                 }),
                  map.end());
    }

    /// The variant chain of `instance`'s source (one element for a plain prefab).
    bool ChainOf(const PrefabInstance* instance, std::vector<PrefabAsset*>& out_chain)
    {
        out_chain.clear();
        PrefabAsset* source = instance != nullptr ? static_cast<PrefabAsset*>(instance->GetSourcePrefab()) : nullptr;
        return source != nullptr && PrefabUtility::ResolveVariantChain(source, out_chain);
    }

    /// The base GameObject a SOURCE component hangs off. A base component knows it
    /// (m_ParentObject, bound when the template graph was prepared). A Variant's
    /// added-component template does not — it lives in the variant file and its host
    /// in the base file — so its host comes from the chain's addition record.
    GameObject* SourceHostOf(const PrefabInstance* instance, Component* source)
    {
        if (source == nullptr)
            return nullptr;
        if (GameObject* host = source->GetParentObject())
            return host;
        std::vector<PrefabAsset*> chain;
        if (!ChainOf(instance, chain))
            return nullptr;
        PrefabUtility::VariantStructure structure;
        PrefabUtility::ResolveVariantStructure(chain, structure);
        for (const auto& [component, host] : structure.added)
        {
            if (component == source)
                return host;
        }
        return nullptr;
    }

    /// Clone `source` (a base component or a variant's added-component template) onto
    /// `instance_host`, carrying the value the instance should see — the source PLUS
    /// the variant chain's overrides on it — and pair it in the corresponding-source
    /// map. This is what every structural re-introduction goes through (Unsuppress,
    /// Revert, Merge, InstantiateVariant), so they all agree on the resulting state.
    Component* IntroduceComponentFromSource(PrefabInstance* instance, Component& source, GameObject& instance_host)
    {
        Object* clone_obj = TransferUtility::CloneObjectViaSerialization(source);  // assigns the instance ID
        Component* clone = clone_obj != nullptr ? Cast<Component>(clone_obj) : nullptr;
        if (clone == nullptr)
        {
            LOG_ERROR(ZPrefab, "IntroduceComponentFromSource: cloning a {} failed", source.GetTypeName());
            return nullptr;
        }
        clone->PostLoadResource(&instance_host);
        instance_host.addComponent(clone);

        // The chain may override fields of this very component; a plain clone would
        // carry the bare source value.
        std::vector<PropertyModification> layer;
        VariantLayerOf(instance, layer);
        const int32_t source_id = source.GetInstanceID();
        const bool overridden = std::any_of(layer.begin(), layer.end(), [source_id](const PropertyModification& m) {
            return m.target.GetInstanceID() == source_id;
        });
        if (overridden)
        {
            prefab::SerialisedObject tree;
            std::vector<uint8_t> bytes;
            if (PrefabUtility::SnapshotWithOverrides(source, layer, tree, bytes))
                ReadBackInto(*clone, bytes);
        }

        PrefabInstance::CorrespondingSourceEntry entry;
        entry.instance_object = PPtr<Object>(clone);
        entry.source_object = PPtr<Object>(&source);
        instance->GetCorrespondingSourceMap().push_back(entry);
        return clone;
    }

    /// Find the existing modification entry for (target, path), or nullptr.
    PropertyModification* FindModification(PrefabInstance* instance, PPtr<Object> target, const eastl::string& path)
    {
        for (auto& mod : instance->GetModifications())
        {
            if (mod.target.GetInstanceID() == target.GetInstanceID() && mod.property_path == path)
                return &mod;
        }
        return nullptr;
    }

}  // namespace

PrefabInstance* PrefabUtilityEditor::InstantiateAsPrefab(const std::filesystem::path& prefab_path)
{
    // Whole-graph load (see PrefabUtility::LoadPrefabAsset): a single-object
    // ReadObject<PrefabAsset> never produced the root GameObject, so this failed
    // for every prefab on disk.
    PrefabAsset* asset = PrefabUtility::LoadPrefabAsset(prefab_path);
    if (asset == nullptr)
    {
        LOG_ERROR(ZPrefab, "InstantiateAsPrefab: failed to read PrefabAsset from {}", prefab_path.generic_string());
        return nullptr;
    }
    return InstantiateAsPrefab(asset);
}

PrefabInstance* PrefabUtilityEditor::InstantiateAsPrefab(PrefabAsset* asset)
{
    if (asset == nullptr)
    {
        LOG_ERROR(ZPrefab, "InstantiateAsPrefab(asset): null asset");
        return nullptr;
    }

    GameObject* source_root = asset->GetRootGameObject();
    if (source_root == nullptr)
    {
        LOG_ERROR(ZPrefab, "InstantiateAsPrefab(asset): asset has no root GameObject");
        return nullptr;
    }

    prefab_clone::CloneResult clone = prefab_clone::CloneSubtree(source_root);
    if (!clone.valid())
    {
        LOG_ERROR(ZPrefab, "InstantiateAsPrefab: structural clone failed");
        return nullptr;
    }

    // Drive postLoadResource on the clone subtree so each Component's
    // m_ParentObject back-pointer is bound to the clone-side GameObject.
    for (Object* clone_obj : clone.clones)
    {
        if (auto* clone_go = Cast<GameObject>(clone_obj))
        {
            for (ImmediatePtr<Component>& comp_ptr : clone_go->getComponents())
            {
                if (Component* comp = comp_ptr)
                {
                    comp->PostLoadResource(clone_go);
                }
            }
        }
    }

    PrefabInstance* instance = GET_SYSTEM(ObjectManager)->NewObject<PrefabInstance>();
    if (instance == nullptr)
    {
        LOG_ERROR(ZPrefab, "InstantiateAsPrefab: ObjectManager failed to allocate PrefabInstance");
        return nullptr;
    }
    GET_SYSTEM(ObjectManager)->AllocateAndAssignInstanceID(instance);

    instance->SetSourcePrefab(PPtr<PrefabAsset>(asset));
    instance->SetRootGameObject(PPtr<GameObject>(clone.clone_root));

    auto& source_map = instance->GetCorrespondingSourceMap();
    source_map.clear();
    source_map.reserve(clone.sources.size());
    for (size_t i = 0; i < clone.sources.size(); ++i)
    {
        PrefabInstance::CorrespondingSourceEntry entry;
        entry.instance_object = PPtr<Object>(clone.clones[i]);
        entry.source_object = PPtr<Object>(clone.sources[i]);
        source_map.push_back(entry);
    }

    LOG_INFO(ZPrefab,
             "InstantiateAsPrefab: instantiated '{}' — {} corresponding-source pairs",
             asset->name.c_str(),
             static_cast<int>(source_map.size()));
    return instance;
}

bool PrefabUtilityEditor::RecordPropertyOverride(PrefabInstance* instance,
                                                 PPtr<Object> target,
                                                 const eastl::string& path,
                                                 const std::vector<uint8_t>& value_bytes)
{
    if (instance == nullptr || target.IsNull() || path.empty())
        return false;

    // Validate that the path resolves on the target — fail-fast so callers
    // don't end up with a list of overrides referencing dead paths.
    Object* obj = ResolveInstanceObject(instance, target);
    if (obj == nullptr)
    {
        LOG_WARNING(ZPrefab, "RecordPropertyOverride: target not found in PrefabInstance");
        return false;
    }
    prefab::SerialisedObject snap;
    std::vector<uint8_t> snap_bytes;
    if (!SnapshotObject(*obj, snap, snap_bytes))
        return false;

    prefab::PropertyLocation loc;
    if (!prefab::LocatePropertyInStream(snap.type_tree, snap_bytes, path, loc))
    {
        LOG_WARNING(ZPrefab, "RecordPropertyOverride: path '{}' does not exist on target — {}", path.c_str(),
                    prefab::ExplainPathFailure(snap.type_tree, path).c_str());
        return false;
    }
    if (!loc.valid() || loc.byte_size != static_cast<int64_t>(value_bytes.size()))
    {
        LOG_WARNING(ZPrefab,
                    "RecordPropertyOverride: value_bytes size {} != field size {} for path '{}'",
                    static_cast<int>(value_bytes.size()),
                    static_cast<int>(loc.byte_size),
                    path.c_str());
        return false;
    }

    // Insert / overwrite.
    if (PropertyModification* existing = FindModification(instance, target, path))
    {
        existing->value_bytes = value_bytes;
        existing->object_reference = PPtr<Object>();
        return true;
    }
    PropertyModification mod;
    mod.target = target;
    mod.property_path = path;
    mod.value_bytes = value_bytes;
    instance->AddModification(mod);
    return true;
}

bool PrefabUtilityEditor::RecordPropertyOverride(PrefabInstance* instance,
                                                 PPtr<Object> target,
                                                 const eastl::string& path,
                                                 PPtr<Object> object_reference)
{
    // PPtr fields occupy 4 bytes (the InstanceID). Encode and forward to the
    // value_bytes path so the apply pipeline only handles one shape.
    return RecordPropertyOverride(instance, target, path, EncodePPtrAsBytes(object_reference));
}

bool PrefabUtilityEditor::RecordCurrentValueAsOverride(PrefabInstance* instance,
                                                       PPtr<Object> target,
                                                       const eastl::string& path)
{
    if (instance == nullptr || target.IsNull() || path.empty())
        return false;

    Object* obj = ResolveInstanceObject(instance, target);
    if (obj == nullptr)
        return false;

    prefab::SerialisedObject snap;
    std::vector<uint8_t> snap_bytes;
    if (!SnapshotObject(*obj, snap, snap_bytes))
        return false;

    prefab::PropertyLocation loc;
    if (!prefab::LocatePropertyInStream(snap.type_tree, snap_bytes, path, loc))
    {
        LOG_WARNING(ZPrefab, "RecordCurrentValueAsOverride: path '{}' not found — {}", path.c_str(),
                    prefab::ExplainPathFailure(snap.type_tree, path).c_str());
        return false;
    }
    if (!loc.valid() || loc.byte_size <= 0)
        return false;

    std::vector<uint8_t> current_bytes(
        snap_bytes.begin() + loc.byte_offset,
        snap_bytes.begin() + loc.byte_offset + loc.byte_size);
    return RecordPropertyOverride(instance, target, path, current_bytes);
}

int PrefabUtilityEditor::ApplyAll(PrefabInstance* instance)
{
    if (instance == nullptr)
        return 0;

    int applied = 0;
    for (const auto& mod : instance->GetModifications())
    {
        // Apply pushes the override INTO the source. Look up the source object.
        PPtr<Object> source_pptr = instance->FindSourceForInstance(mod.target);
        Object* source_obj = static_cast<Object*>(source_pptr);
        if (source_obj == nullptr)
        {
            LOG_WARNING(ZPrefab, "ApplyAll: no source counterpart for instance target — skip");
            continue;
        }

        // PPtr-typed override: encode the InstanceID; otherwise use raw bytes.
        if (!mod.object_reference.IsNull())
        {
            const std::vector<uint8_t> bytes = EncodePPtrAsBytes(mod.object_reference);
            if (SpliceFieldValueInto(*source_obj, mod.property_path, &bytes, nullptr, nullptr))
                ++applied;
        }
        else if (!mod.value_bytes.empty())
        {
            if (SpliceFieldValueInto(*source_obj, mod.property_path, &mod.value_bytes, nullptr, nullptr))
                ++applied;
        }
        else
        {
            LOG_WARNING(ZPrefab,
                        "ApplyAll: modification has no value_bytes nor object_reference (path '{}') — skipped",
                        mod.property_path.c_str());
        }
    }

    if (applied > 0)
        instance->ClearModifications();

    LOG_INFO(ZPrefab, "ApplyAll: {} / {} modifications applied", applied, static_cast<int>(instance->GetModifications().size() + applied));
    return applied;
}

int PrefabUtilityEditor::Revert(PrefabInstance* instance)
{
    if (instance == nullptr)
        return 0;

    int reverted = 0;
    // Snapshot every distinct source object once so each Revert call doesn't
    // reserialise the same source repeatedly (common case: many overrides on
    // the same component).
    for (const auto& mod : instance->GetModifications())
    {
        Object* instance_obj = ResolveInstanceObject(instance, mod.target);
        if (instance_obj == nullptr)
            continue;

        PPtr<Object> source_pptr = instance->FindSourceForInstance(mod.target);
        Object* source_obj = static_cast<Object*>(source_pptr);
        if (source_obj == nullptr)
        {
            LOG_WARNING(ZPrefab, "Revert: no source counterpart for instance target — skip");
            continue;
        }

        // Base value + the variant's own overrides (never spliced into source_obj —
        // it is shared). Reverting to the bare base would undo the whole variant layer.
        prefab::SerialisedObject src_tree;
        std::vector<uint8_t> src_bytes;
        if (!SnapshotEffectiveSource(*source_obj, instance, src_tree, src_bytes))
            continue;

        if (SpliceFieldValueInto(*instance_obj, mod.property_path, nullptr, &src_bytes, &src_tree))
            ++reverted;
    }

    instance->ClearModifications();

    // ---- Structural overrides ----------------------------------------------------
    // Components the instance added go away; components it suppressed come back.
    // The re-introduction is ReconcileStructure's (same path as Merge and
    // InstantiateVariant), so a suppressed component returns with the value the
    // source — and the Variant chain — gives it, not a stale copy.
    for (const PPtr<Component>& entry : instance->GetAddedComponents())
    {
        if (Component* added = static_cast<Component*>(entry))
        {
            if (GameObject* host = added->GetParentObject())
                host->EditorRemoveComponent(added);
            ++reverted;
        }
    }
    instance->GetAddedComponents().clear();
    reverted += static_cast<int>(instance->GetRemovedComponents().size());
    instance->GetRemovedComponents().clear();
    ReconcileStructure(instance);

    LOG_INFO(ZPrefab, "Revert: {} modifications reverted", reverted);
    return reverted;
}

bool PrefabUtilityEditor::RevertSingleOverride(PrefabInstance* instance,
                                               PPtr<Object> target,
                                               const eastl::string& path)
{
    if (instance == nullptr || target.IsNull() || path.empty())
        return false;

    Object* instance_obj = ResolveInstanceObject(instance, target);
    if (instance_obj == nullptr)
        return false;

    PPtr<Object> source_pptr = instance->FindSourceForInstance(target);
    Object* source_obj = static_cast<Object*>(source_pptr);
    if (source_obj == nullptr)
        return false;

    // Base value + the variant's own overrides (never spliced into source_obj).
    prefab::SerialisedObject src_tree;
    std::vector<uint8_t> src_bytes;
    if (!SnapshotEffectiveSource(*source_obj, instance, src_tree, src_bytes))
        return false;

    if (!SpliceFieldValueInto(*instance_obj, path, nullptr, &src_bytes, &src_tree))
        return false;

    // Drop the matching entry from m_Modifications, if present.
    auto& mods = instance->GetModifications();
    for (auto it = mods.begin(); it != mods.end(); ++it)
    {
        if (it->target.GetInstanceID() == target.GetInstanceID() && it->property_path == path)
        {
            mods.erase(it);
            break;
        }
    }
    return true;
}

// =====================================================================================
// Phase 2b-4d: Added / Removed Components API
// -------------------------------------------------------------------------------------
// Mirrors Unity's PrefabUtility "added components" / "removed components" override
// concept. Two distinct cases:
//
//   * Added component   — a Component instantiated only in this scene/instance,
//                         with no source counterpart. Tracked in
//                         instance.m_AddedComponents. Goes onto the host
//                         GameObject's m_Components like any other component.
//
//   * Removed component — a source-side Component that this instance suppresses.
//                         The instance-side counterpart (looked up via the
//                         corresponding-source map) is detached from its host
//                         GameObject; the SOURCE-side PPtr is recorded in
//                         instance.m_RemovedComponents.
//
// Apply-time semantics (Phase 3 will tie these in):
//   * Added components apply by cloning into the source PrefabAsset.
//   * Removed components apply by deleting that Component from the source.
// =====================================================================================

bool PrefabUtilityEditor::AddComponentOverride(PrefabInstance* instance,
                                               PPtr<GameObject> host_go,
                                               Component* component)
{
    if (instance == nullptr || host_go.IsNull() || component == nullptr)
        return false;

    GameObject* host = static_cast<GameObject*>(host_go);
    if (host == nullptr)
    {
        LOG_WARNING(ZPrefab, "AddComponentOverride: host GameObject pointer is null");
        return false;
    }

    // Bind the component's m_ParentObject back-pointer so subsequent runtime
    // queries (Component::GetParentObject etc.) work against the host. This is
    // the same call site CloneSubtree uses after a structural clone.
    component->PostLoadResource(host);

    // Append to the GameObject's component list.
    host->addComponent(component);

    // Record the override in PrefabInstance bookkeeping.
    auto& added = instance->GetAddedComponents();
    PPtr<Component> entry(component);
    // Avoid duplicates if the editor calls this twice.
    for (const auto& p : added)
    {
        if (p.GetInstanceID() == entry.GetInstanceID())
            return true;
    }
    added.push_back(entry);

    LOG_INFO(ZPrefab,
             "AddComponentOverride: component {} attached to GameObject '{}'",
             component->GetType()->GetName(),
             host->GetName().c_str());
    return true;
}

bool PrefabUtilityEditor::RemoveAddedComponent(PrefabInstance* instance,
                                               PPtr<GameObject> host_go,
                                               Component* component)
{
    if (instance == nullptr || host_go.IsNull() || component == nullptr)
        return false;

    GameObject* host = static_cast<GameObject*>(host_go);
    if (host == nullptr)
        return false;

    // Detach from the host's component list.
    if (!host->EditorRemoveComponent(component))
    {
        LOG_WARNING(ZPrefab, "RemoveAddedComponent: component not found on host GameObject");
        // continue — still try to drop the bookkeeping entry, since the
        // instance.m_AddedComponents listing might be stale.
    }

    // Drop from m_AddedComponents.
    auto& added = instance->GetAddedComponents();
    int32_t target_id = PPtr<Component>(component).GetInstanceID();
    for (auto it = added.begin(); it != added.end(); ++it)
    {
        if (it->GetInstanceID() == target_id)
        {
            added.erase(it);
            return true;
        }
    }
    LOG_WARNING(ZPrefab, "RemoveAddedComponent: component was not in m_added_components");
    return false;
}

bool PrefabUtilityEditor::SuppressSourceComponent(PrefabInstance* instance,
                                                  PPtr<Component> source_component)
{
    if (instance == nullptr || source_component.IsNull())
        return false;

    // 1) Look up the instance-side counterpart through the corresponding-source map.
    //    PPtr<Component> → PPtr<Object> via the templated converting constructor.
    PPtr<Object> instance_pptr = instance->FindInstanceForSource(PPtr<Object>(source_component));
    if (instance_pptr.IsNull())
    {
        LOG_WARNING(ZPrefab,
                    "SuppressSourceComponent: source_component has no instance-side counterpart "
                    "(was it already suppressed, or does it belong to a different PrefabInstance?)");
        return false;
    }

    Component* instance_comp = Cast<Component>(static_cast<Object*>(instance_pptr));
    if (instance_comp == nullptr)
    {
        LOG_WARNING(ZPrefab, "SuppressSourceComponent: instance counterpart is not a Component");
        return false;
    }

    // 2) Detach from its owning instance GameObject. The host is reachable via
    //    Component::m_ParentObject (set during postLoadResource at clone time).
    GameObject* host = instance_comp->GetParentObject();
    if (host == nullptr)
    {
        LOG_WARNING(ZPrefab, "SuppressSourceComponent: instance Component has no parent GameObject");
    }
    else
    {
        host->EditorRemoveComponent(instance_comp);
    }

    // The detached clone is no longer the instance's counterpart of the source
    // component. Keeping the pair (as this used to) meant "has a counterpart" no
    // longer implied "is attached" — and Unsuppress then added a SECOND pair for the
    // same source, so one suppress/unsuppress round trip left it paired twice.
    ErasePairForInstanceObject(instance, instance_comp);

    // 3) Append source PPtr to m_RemovedComponents (skip duplicates).
    auto& removed = instance->GetRemovedComponents();
    int32_t src_id = source_component.GetInstanceID();
    for (const auto& p : removed)
    {
        if (p.GetInstanceID() == src_id)
            return true;
    }
    removed.push_back(source_component);

    LOG_INFO(ZPrefab,
             "SuppressSourceComponent: source Component (instance_id={}) suppressed on instance",
             src_id);
    return true;
}

bool PrefabUtilityEditor::UnsuppressSourceComponent(PrefabInstance* instance,
                                                    PPtr<Component> source_component)
{
    if (instance == nullptr || source_component.IsNull())
        return false;

    // 1) Verify it's actually currently suppressed.
    auto& removed = instance->GetRemovedComponents();
    int32_t src_id = source_component.GetInstanceID();
    auto it = removed.begin();
    for (; it != removed.end(); ++it)
    {
        if (it->GetInstanceID() == src_id)
            break;
    }
    if (it == removed.end())
    {
        LOG_WARNING(ZPrefab, "UnsuppressSourceComponent: component is not in m_removed_components");
        return false;
    }

    // 2) Find the source GameObject that owns this Component (the source-side
    //    Component's m_ParentObject), then look up its instance-side counterpart
    //    via the corresponding-source map → that's the host where we attach the
    //    re-introduced clone.
    Component* src_comp = static_cast<Component*>(source_component);
    if (src_comp == nullptr)
    {
        LOG_WARNING(ZPrefab, "UnsuppressSourceComponent: source PPtr could not be dereferenced");
        return false;
    }
    GameObject* src_host = SourceHostOf(instance, src_comp);
    if (src_host == nullptr)
    {
        LOG_WARNING(ZPrefab, "UnsuppressSourceComponent: source Component has no parent GameObject");
        return false;
    }
    PPtr<Object> instance_host_pptr = instance->FindInstanceForSource(PPtr<Object>(src_host));
    if (instance_host_pptr.IsNull())
    {
        LOG_WARNING(ZPrefab,
                    "UnsuppressSourceComponent: source GameObject has no instance counterpart "
                    "(prefab structure changed since instantiation?)");
        return false;
    }
    GameObject* instance_host = Cast<GameObject>(static_cast<Object*>(instance_host_pptr));
    if (instance_host == nullptr)
        return false;

    // 3) Clone the source Component onto the instance host, with the variant chain's
    //    overrides on it, and pair it in the corresponding-source map so later
    //    property overrides on it still resolve apply/revert. (This used to clone the
    //    bare source value — on a Variant instance that dropped the variant's own
    //    overrides on the re-introduced component.)
    if (IntroduceComponentFromSource(instance, *src_comp, *instance_host) == nullptr)
        return false;

    // 4) Drop from m_RemovedComponents.
    removed.erase(it);

    LOG_INFO(ZPrefab,
             "UnsuppressSourceComponent: source Component (instance_id={}) re-introduced on instance",
             src_id);
    return true;
}

// =====================================================================================
// Phase 3c: Unpack
// =====================================================================================

GameObject* PrefabUtilityEditor::UnpackPrefabInstance(PrefabInstance* instance)
{
    if (instance == nullptr)
        return nullptr;

    GameObject* kept_root = static_cast<GameObject*>(instance->GetRootGameObject());
    if (kept_root == nullptr)
    {
        LOG_WARNING(ZPrefab, "UnpackPrefabInstance: instance has no root GameObject");
        return nullptr;
    }

    // Drop every piece of bookkeeping. The cloned subtree lives on as ordinary
    // scene-owned GameObjects/Components — the SerializedFile they belong to
    // is unchanged, only the PrefabInstance's view of them is severed.
    instance->ClearModifications();
    instance->GetCorrespondingSourceMap().clear();
    instance->GetAddedComponents().clear();
    instance->GetRemovedComponents().clear();
    instance->SetSourcePrefab(PPtr<PrefabAsset>());
    instance->SetRootGameObject(PPtr<GameObject>());

    LOG_INFO(ZPrefab,
             "UnpackPrefabInstance: prefab linkage severed; root '{}' kept as plain subtree",
             kept_root->GetName().c_str());
    return kept_root;
}

// =====================================================================================
// Phase 3b: Variant instantiation
// -------------------------------------------------------------------------------------
// We walk the variant chain bottom-up to find the deepest non-variant base, then
// instantiate it once via InstantiateAsPrefab (which seeds the corresponding-source
// map). Then we apply every link's overrides on top, base -> ... -> variant, so a
// nearer link wins on the same field.
//
// 2026-10-27: the old comment here said "we no longer need the Variant ancestor's
// modifications to be stored on PrefabAsset". That was wrong and it is why Variant
// was dead code — nothing ever applied them. Modifications now live in
// PrefabAsset::m_variant_overrides (NOT on PrefabInstance, which is
// kTypeIsEditorOnly and therefore invisible to the player build), and this function
// applies them through the same Runtime helper `PrefabUtility::Instantiate` uses.
//
// The Variant asset pointer becomes the `m_SourcePrefab` of the resulting
// PrefabInstance, so future Apply/Revert calls target the Variant, not its base.
// =====================================================================================

PrefabInstance* PrefabUtilityEditor::InstantiateVariant(PrefabAsset* variant)
{
    if (variant == nullptr)
    {
        LOG_ERROR(ZPrefab, "InstantiateVariant: null variant asset");
        return nullptr;
    }
    if (!variant->IsVariant())
    {
        // Plain prefab — defer to the regular path.
        return InstantiateAsPrefab(variant);
    }

    // Walk to the deepest non-variant base. Each link is a PPtr<PrefabAsset>;
    // resolve it through the operator T*() conversion.
    PrefabAsset* deepest = variant;
    while (deepest->IsVariant())
    {
        PrefabAsset* base = static_cast<PrefabAsset*>(deepest->GetVariantBase());
        if (base == nullptr)
        {
            LOG_ERROR(ZPrefab,
                      "InstantiateVariant: variant chain broken — '{}' marked variant but base PPtr is null",
                      deepest->name.c_str());
            return nullptr;
        }
        deepest = base;
    }

    // Instantiate from the deepest base. This produces a fresh PrefabInstance
    // whose corresponding-source map pairs every clone with the BASE's source.
    PrefabInstance* instance = InstantiateAsPrefab(deepest);
    if (instance == nullptr)
        return nullptr;

    // Apply the chain's overrides on top. Every link's `target` names an object in
    // the DEEPEST base's graph, which is exactly what the corresponding-source map
    // is keyed by — so one map serves the whole chain.
    std::vector<PrefabAsset*> chain;
    if (!PrefabUtility::ResolveVariantChain(variant, chain))
    {
        LOG_ERROR(ZPrefab, "InstantiateVariant: '{}' has a broken variant chain — nothing instantiated",
                  variant->name.c_str());
        return nullptr;
    }

    // Re-point m_SourcePrefab at the variant first: ReconcileStructure reads the
    // chain from it.
    instance->SetSourcePrefab(PPtr<PrefabAsset>(variant));

    // Structure first, same as Runtime's Instantiate. In the editor this goes through
    // ReconcileStructure rather than PrefabUtility::ApplyVariantStructuralChanges: it
    // applies the same net structure (ResolveVariantStructure) but ALSO pairs each
    // added component's clone with its template in corresponding_source — without
    // that pair the clone is invisible to Suppress, to overrides on it and to Revert.
    const int structural = ReconcileStructure(instance);

    std::unordered_map<Object*, Object*> source_to_clone;
    for (const auto& entry : instance->GetCorrespondingSourceMap())
    {
        Object* source = static_cast<Object*>(entry.source_object);
        Object* clone = static_cast<Object*>(entry.instance_object);
        if (source != nullptr && clone != nullptr)
        {
            source_to_clone[source] = clone;
        }
    }
    // Includes overrides whose target is a variant-added template: the map now
    // pairs those too.
    const int applied = structural + PrefabUtility::ApplyVariantOverrides(chain, source_to_clone);

    LOG_INFO(ZPrefab,
             "InstantiateVariant: instantiated '{}' (base='{}'); {} variant override(s) and "
             "{} structural change(s) applied; "
             "source_prefab=variant, corresponding_source=base",
             variant->name.c_str(),
             deepest->name.c_str(),
             applied - structural,
             structural);
    return instance;
}

// =====================================================================================
// Apply to a Variant
// -------------------------------------------------------------------------------------
// See the header for why the plain ApplyAll must not be used here. Short version: the
// source objects of a Variant instance ARE the base's objects, and the base is shared.
// =====================================================================================

int PrefabUtilityEditor::ApplyAllToVariant(PrefabInstance* instance, bool include_structure)
{
    if (instance == nullptr)
    {
        return 0;
    }

    PrefabAsset* variant = static_cast<PrefabAsset*>(instance->GetSourcePrefab());
    if (variant == nullptr || !variant->IsVariant())
    {
        LOG_ERROR(ZPrefab, "ApplyAllToVariant: the instance's source prefab is not a Variant");
        return 0;
    }

    // Rolled back if the file cannot be written (see the failure branch below).
    const std::vector<PropertyModification>      saved_overrides = variant->GetVariantOverrides();
    const std::vector<VariantComponentAddition>  saved_added     = variant->GetVariantAddedComponents();
    const std::vector<PPtr<Component>>           saved_removed   = variant->GetVariantRemovedComponents();

    int applied = 0;
    for (const PropertyModification& mod : instance->GetModifications())
    {
        if (mod.object_reference.IsNull() && mod.value_bytes.empty())
        {
            LOG_WARNING(ZPrefab, "ApplyAllToVariant: '{}' has neither a value nor a reference — skipped",
                        mod.property_path.c_str());
            continue;
        }

        // The override must name the object's SOURCE — a base object, or the template
        // of a component some link of the chain added (InstantiateVariant pairs those
        // too). Either is resolvable when the variant is instantiated: Runtime's
        // ptr_map gains template -> clone in ApplyVariantStructuralChanges.
        PPtr<Object> base_object = instance->FindSourceForInstance(mod.target);
        if (base_object.IsNull())
        {
            LOG_WARNING(ZPrefab, "ApplyAllToVariant: no base counterpart for '{}' — skipped",
                        mod.property_path.c_str());
            continue;
        }

        PropertyModification recorded = mod;
        recorded.target = base_object;

        // Same (target, path) replaces: a second Apply of the same field must not
        // leave two entries fighting over one slot.
        auto& overrides = variant->GetVariantOverrides();
        bool replaced = false;
        for (PropertyModification& existing : overrides)
        {
            if (existing.target.GetInstanceID() == recorded.target.GetInstanceID() &&
                existing.property_path == recorded.property_path)
            {
                existing = recorded;
                replaced = true;
                break;
            }
        }
        if (!replaced)
        {
            overrides.push_back(recorded);
        }
        ++applied;
    }

    // ---- Structural changes: components this instance added / suppressed --------
    //
    // Same translation as the property half: `m_added_components` names objects on
    // the INSTANCE and `m_removed_components` already names source components (a
    // base component, or a template some link of the chain added), so both have to be
    // re-expressed against the chain before they can be stored on the variant.
    //
    // Single-field Apply passes include_structure=false: before this flag existed,
    // applying ONE field from the Inspector silently committed every added / removed
    // component of the instance too.
    std::vector<std::pair<Component*, Component*>> adopted;  // (instance component, new template)
    auto& variant_added = variant->GetVariantAddedComponents();
    auto& variant_removed = variant->GetVariantRemovedComponents();
    for (const PPtr<Component>& entry : include_structure ? instance->GetAddedComponents()
                                                          : std::vector<PPtr<Component>>{})
    {
        Component* added = static_cast<Component*>(entry);
        if (added == nullptr)
        {
            continue;
        }
        GameObject* host = added->GetParentObject();
        if (host == nullptr)
        {
            LOG_WARNING(ZPrefab, "ApplyAllToVariant: an added component has no host GameObject — skipped");
            continue;
        }
        PPtr<Object> base_host_object = instance->FindSourceForInstance(PPtr<Object>(host));
        GameObject* base_host = (base_host_object.IsNull())
            ? nullptr
            : Cast<GameObject>(static_cast<Object*>(base_host_object));
        if (base_host == nullptr)
        {
            LOG_WARNING(ZPrefab, "ApplyAllToVariant: no base counterpart for host '{}' — skipped",
                        host->name.c_str());
            continue;
        }

        // The variant FILE needs its own copy: this component belongs to the scene's
        // SerializedFile and must not be referenced across files.
        // (CloneObjectViaSerialization assigns the instance ID itself; A7 assigned a
        // second one here, leaving a dangling ID-table entry behind.)
        Object* clone_object = TransferUtility::CloneObjectViaSerialization(*added);
        Component* clone_component = (clone_object != nullptr) ? Cast<Component>(clone_object) : nullptr;
        if (clone_component == nullptr)
        {
            LOG_WARNING(ZPrefab, "ApplyAllToVariant: cloning the added component failed — skipped");
            continue;
        }

        VariantComponentAddition addition;
        addition.host      = PPtr<GameObject>(base_host);
        addition.component = clone_component;
        variant_added.push_back(addition);
        adopted.emplace_back(added, clone_component);
        ++applied;
    }

    for (const PPtr<Component>& entry : include_structure ? instance->GetRemovedComponents()
                                                          : std::vector<PPtr<Component>>{})
    {
        if (entry.IsNull())
        {
            continue;
        }
        const int32_t id = entry.GetInstanceID();

        // Removing a component THIS variant added: drop the addition. Recording a
        // removal would do nothing — within one link removals resolve before
        // additions — and the component would come straight back.
        const auto own = std::find_if(variant_added.begin(), variant_added.end(),
                                      [id](const VariantComponentAddition& a) {
                                          const Component* c = a.component;
                                          return c != nullptr && c->GetInstanceID() == id;
                                      });
        if (own != variant_added.end())
        {
            variant_added.erase(own);
            ++applied;
            continue;
        }

        const bool already = std::any_of(variant_removed.begin(), variant_removed.end(),
                                         [id](const PPtr<Component>& p) { return p.GetInstanceID() == id; });
        if (!already)
        {
            variant_removed.push_back(entry);
            ++applied;
        }
    }

    if (applied == 0)
    {
        return 0;
    }

    AssetManager* asset_manager = GET_SYSTEM(AssetManager);
    std::filesystem::path path;
    int64_t header_file_id = 0;
    const bool has_identity = asset_manager != nullptr &&
                              asset_manager->TryGetIdentityForInstance(variant->GetInstanceID(), path, header_file_id) &&
                              !path.empty();
    if (!has_identity || !PrefabUtility::SaveVariantAsset(variant, path))
    {
        LOG_ERROR(ZPrefab, "ApplyAllToVariant: {} — {} change(s) left unsaved",
                  has_identity ? "writing '" + path.generic_string() + "' failed"
                               : std::string("the variant asset has no file identity"),
                  applied);
        // Put the variant back the way it was: the instance keeps its overrides, so
        // nothing is lost — but a half-applied variant in memory would diverge from
        // its file and leak into every other instance on the next Merge.
        variant->GetVariantOverrides() = saved_overrides;
        variant->GetVariantAddedComponents() = saved_added;
        variant->GetVariantRemovedComponents() = saved_removed;
        return 0;
    }

    // Unity: after Apply the instance no longer overrides anything — the variant now
    // carries those values and that structure itself.
    instance->ClearModifications();
    if (include_structure)
    {
        // The live component the user added is now the instance's counterpart of the
        // variant's new template. Without this pair the next ReconcileStructure (a
        // Merge, a Revert) would see a variant addition with no counterpart and clone
        // a SECOND copy onto the host.
        for (const auto& [instance_component, template_component] : adopted)
        {
            PrefabInstance::CorrespondingSourceEntry pair;
            pair.instance_object = PPtr<Object>(instance_component);
            pair.source_object = PPtr<Object>(template_component);
            instance->GetCorrespondingSourceMap().push_back(pair);
        }
        instance->GetAddedComponents().clear();
        instance->GetRemovedComponents().clear();
    }

    LOG_INFO(ZPrefab, "ApplyAllToVariant: {} change(s) recorded on '{}' "
                      "({} override(s), {} added / {} removed component(s))",
             applied, path.generic_string(),
             static_cast<int>(variant->GetVariantOverrides().size()),
             static_cast<int>(variant->GetVariantAddedComponents().size()),
             static_cast<int>(variant->GetVariantRemovedComponents().size()));
    return applied;
}

namespace
{
    // Every object of a plain prefab's source graph: the root GameObject, its components, and
    // transitively each child Transform's GameObject — the same set SaveSourcePrefab writes.
    void CollectSourceGraphObjects(PrefabAsset* source, std::vector<Object*>& out)
    {
        if (source == nullptr)
        {
            return;
        }
        std::vector<GameObject*> pending;
        if (GameObject* root = source->GetRootGameObject())
        {
            pending.push_back(root);
        }
        while (!pending.empty())
        {
            GameObject* go = pending.back();
            pending.pop_back();
            if (go == nullptr)
            {
                continue;
            }
            out.push_back(go);
            Transform* transform = nullptr;
            for (ImmediatePtr<Component>& entry : go->EditorGetComponentsRef())
            {
                Component* component = entry;
                if (component == nullptr)
                {
                    continue;
                }
                out.push_back(component);
                if (transform == nullptr)
                {
                    transform = Cast<Transform>(component);
                }
            }
            if (transform == nullptr)
            {
                continue;
            }
            for (uint32_t i = 0; i < transform->GetChildCount(); ++i)
            {
                Transform* child = transform->GetChild(i);
                if (child != nullptr && child->GetParentObject() != nullptr)
                {
                    pending.push_back(child->GetParentObject());
                }
            }
        }
    }
}  // namespace

int PrefabUtilityEditor::ApplyStructureToSource(PrefabInstance* instance, bool* out_saved)
{
    if (out_saved != nullptr)
    {
        *out_saved = false;
    }
    if (instance == nullptr)
    {
        return 0;
    }
    PrefabAsset* source = static_cast<PrefabAsset*>(instance->GetSourcePrefab());
    // A Variant records structure in its own file (ApplyAllToVariant); only a plain prefab's
    // source graph is edited in place.
    if (source == nullptr || source->IsVariant())
    {
        return 0;
    }
    if (instance->GetAddedComponents().empty() && instance->GetRemovedComponents().empty())
    {
        return 0;
    }

    AssetManager* asset_manager = GET_SYSTEM(AssetManager);
    ObjectManager* object_manager = GET_SYSTEM(ObjectManager);
    std::filesystem::path path;
    int64_t header_file_id = 0;
    if (asset_manager == nullptr || object_manager == nullptr ||
        !asset_manager->TryGetIdentityForInstance(source->GetInstanceID(), path, header_file_id) || path.empty())
    {
        LOG_WARNING(ZPrefab, "ApplyStructureToSource: the source prefab has no file identity — structure not applied");
        return 0;
    }

    // Highest fileID this file uses today. New objects must take FRESH fileIDs: every object is
    // written under the fileID it already has (SaveSourcePrefab), so reusing a slot would
    // silently retarget any reference that still points at the old object.
    int64_t highest_file_id = header_file_id;
    {
        std::vector<Object*> graph;
        CollectSourceGraphObjects(source, graph);
        for (const PrefabInstance::CorrespondingSourceEntry& entry : instance->GetCorrespondingSourceMap())
        {
            if (Object* object = static_cast<Object*>(entry.source_object))
            {
                graph.push_back(object);
            }
        }
        for (Object* object : graph)
        {
            std::filesystem::path object_path;
            int64_t file_id = 0;
            if (asset_manager->TryGetIdentityForInstance(object->GetInstanceID(), object_path, file_id) &&
                asset_manager->GetInstanceIDFromPathAndFileID(path, file_id) == object->GetInstanceID() &&
                file_id > highest_file_id)
            {
                highest_file_id = file_id;
            }
        }
    }

    // (instance component, source host, source template) — kept so a failed save can be undone
    // and so the live component can be paired with its new template afterwards.
    struct Attachment
    {
        Component* instance_component;
        GameObject* source_host;
        Component* source_template;
    };
    std::vector<Attachment> attached;
    std::vector<std::pair<GameObject*, Component*>> detached;
    std::vector<GameObject*> touched_hosts;
    int applied = 0;

    // ---- components this instance ADDED: a new object enters the source graph ----
    for (const PPtr<Component>& entry : instance->GetAddedComponents())
    {
        Component* added = static_cast<Component*>(entry);
        if (added == nullptr)
        {
            continue;
        }
        GameObject* host = added->GetParentObject();
        if (host == nullptr)
        {
            LOG_WARNING(ZPrefab, "ApplyStructureToSource: an added component has no host GameObject — skipped");
            continue;
        }
        GameObject* source_host =
            Cast<GameObject>(static_cast<Object*>(instance->FindSourceForInstance(PPtr<Object>(host))));
        if (source_host == nullptr)
        {
            LOG_WARNING(ZPrefab, "ApplyStructureToSource: no source counterpart for host '{}' — skipped",
                        host->name.c_str());
            continue;
        }

        // Claim the next free fileID slot, then Produce the template UNDER the instance ID that
        // (path, fileID) already mints — that is what gives it an identity in this file, which is
        // exactly what SaveSourcePrefab demands of every object it writes.
        int64_t file_id = highest_file_id + 1;
        while (object_manager->IDToPointer(asset_manager->GetInstanceIDFromPathAndFileID(path, file_id)) != nullptr)
        {
            ++file_id;
        }
        highest_file_id = file_id;
        Object* produced = object_manager->Produce(added->GetType(),
                                                   asset_manager->GetInstanceIDFromPathAndFileID(path, file_id));
        Component* component_template = produced != nullptr ? Cast<Component>(produced) : nullptr;
        if (component_template == nullptr)
        {
            LOG_WARNING(ZPrefab, "ApplyStructureToSource: producing the source template failed — skipped");
            continue;
        }
        // Copy the values across. NOT CloneObjectViaSerialization: that picks its own instance ID,
        // and the ID is what has to match the fileID claimed above.
        std::vector<uint8_t> bytes;
        TransferUtility::WriteObjectToVector(*added, bytes);
        if (!bytes.empty())
        {
            TransferUtility::ReadObjectFromVector(*component_template, bytes);
        }

        source_host->addComponent(component_template);
        attached.push_back(Attachment{added, source_host, component_template});
        touched_hosts.push_back(source_host);
        ++applied;
    }

    // ---- components this instance REMOVED: drop them from the source graph ----
    // Entries already name SOURCE components (SuppressSourceComponent records the source side),
    // so unlike the additions these need no translation.
    for (const PPtr<Component>& entry : instance->GetRemovedComponents())
    {
        Component* source_component = static_cast<Component*>(entry);
        if (source_component == nullptr)
        {
            continue;
        }
        GameObject* source_host = source_component->GetParentObject();
        if (source_host == nullptr || !source_host->EditorRemoveComponent(source_component))
        {
            LOG_WARNING(ZPrefab, "ApplyStructureToSource: a removed component is not on its source host — skipped");
            continue;
        }
        // The object stays alive on purpose: other instances still pair their clones with it and
        // Merge has to be able to resolve it. It simply drops out of the graph, and out of the file.
        detached.emplace_back(source_host, source_component);
        touched_hosts.push_back(source_host);
        ++applied;
    }

    if (applied == 0)
    {
        return 0;
    }

    // m_Component (the serialized list) is a separate container from m_Components (the live one)
    // that addComponent / EditorRemoveComponent maintain — without this the file would still
    // carry the old shape.
    std::unordered_set<GameObject*> synced;
    for (GameObject* host : touched_hosts)
    {
        if (host != nullptr && synced.insert(host).second)
        {
            host->SyncSerializedComponents();
        }
    }

    if (!PrefabOverrideModel::SaveSourcePrefab(instance))
    {
        LOG_ERROR(ZPrefab, "ApplyStructureToSource: saving '{}' failed — {} change(s) rolled back",
                  path.generic_string(), applied);
        for (const Attachment& a : attached)
        {
            if (a.source_host != nullptr)
            {
                a.source_host->EditorRemoveComponent(a.source_template);
                a.source_host->SyncSerializedComponents();
            }
        }
        for (const auto& host_template : detached)
        {
            if (host_template.first != nullptr)
            {
                host_template.first->addComponent(host_template.second);
                host_template.first->SyncSerializedComponents();
            }
        }
        // Produce() -> ObjectManager::Destroy(). Not MemoryManager::DestroyObject: Produce uses
        // `new`, and handing that to mi_free is an EXC_BAD_ACCESS on Apple.
        for (const Attachment& a : attached)
        {
            object_manager->Destroy(a.source_template);
        }
        return 0;
    }
    if (out_saved != nullptr)
    {
        *out_saved = true;
    }

    // Unity: after Apply the instance no longer overrides anything — the source now carries that
    // structure itself. Pairing the live component with its new template is what stops the next
    // ReconcileStructure (a Merge, a Revert) from cloning a SECOND copy onto the host.
    for (const Attachment& a : attached)
    {
        PrefabInstance::CorrespondingSourceEntry pair;
        pair.instance_object = PPtr<Object>(a.instance_component);
        pair.source_object = PPtr<Object>(a.source_template);
        instance->GetCorrespondingSourceMap().push_back(pair);
    }
    instance->GetAddedComponents().clear();
    instance->GetRemovedComponents().clear();

    LOG_INFO(ZPrefab, "ApplyStructureToSource: {} structural change(s) applied to '{}' "
                      "({} added / {} removed)",
             applied, path.generic_string(), static_cast<int>(attached.size()), static_cast<int>(detached.size()));
    return applied;
}

PrefabAsset* PrefabUtilityEditor::CreateVariant(PrefabAsset*                 base,
                                                const std::filesystem::path& variant_path)
{
    if (base == nullptr)
    {
        LOG_ERROR(ZPrefab, "CreateVariant: base asset is null");
        return nullptr;
    }
    if (variant_path.empty())
    {
        LOG_ERROR(ZPrefab, "CreateVariant: variant_path is empty");
        return nullptr;
    }

    const eastl::string variant_name(variant_path.stem().string().c_str());

    // The header is only a vehicle for the write: Produce() gives it no
    // path-derived instance ID, so what the asset registry / PPtr resolution
    // needs is the copy that comes back off disk.
    PrefabAsset* variant = PrefabUtility::CreateVariantAsset(base, variant_name);
    if (variant == nullptr)
    {
        return nullptr;
    }

    const bool saved = PrefabUtility::SaveVariantAsset(variant, variant_path);

    // Produce() pairs with ObjectManager::Destroy — pairing it with
    // MemoryManager::DestroyObject crashes in mi_usable_size on Apple.
    GET_SYSTEM(ObjectManager)->Destroy(variant);

    if (!saved)
    {
        return nullptr;
    }

    PrefabAsset* persisted = PrefabUtility::LoadPrefabAsset(variant_path);
    if (persisted == nullptr)
    {
        LOG_ERROR(ZPrefab, "CreateVariant: wrote '{}' but could not read it back",
                  variant_path.generic_string());
        return nullptr;
    }

    LOG_INFO(ZPrefab, "CreateVariant: '{}' is now a variant of '{}'",
             variant_path.generic_string(), base->name.c_str());
    return persisted;
}

// =====================================================================================
// Phase 3d: MergeFromSource — refresh instance against current source state
// -------------------------------------------------------------------------------------
// For every (instance_obj, source_obj) pair we already know about:
//   1. Snapshot the source's current bytes (post any source-side authoring edits).
//   2. Splice in any property-overrides that target THIS instance object.
//   3. Read the merged bytes back into the instance object.
//
// This is the "refresh" gesture in editor UI: the user edited the source prefab
// (or its base, in a Variant chain), saved, and now wants every placed instance
// to pick up the changes — except wherever the instance has its own override.
// =====================================================================================

int PrefabUtilityEditor::ReconcileStructure(PrefabInstance* instance)
{
    if (instance == nullptr)
        return 0;

    std::vector<PrefabAsset*> chain;
    if (!ChainOf(instance, chain))
        return 0;
    PrefabUtility::VariantStructure structure;
    PrefabUtility::ResolveVariantStructure(chain, structure);

    std::unordered_set<int32_t> chain_removed;
    for (Component* c : structure.removed)
        chain_removed.insert(c->GetInstanceID());
    std::unordered_set<int32_t> chain_added;
    for (const auto& [component, host] : structure.added)
        chain_added.insert(component->GetInstanceID());
    std::unordered_set<int32_t> instance_removed;
    for (const PPtr<Component>& p : instance->GetRemovedComponents())
        instance_removed.insert(p.GetInstanceID());
    std::unordered_set<const Object*> instance_added;
    for (const PPtr<Component>& p : instance->GetAddedComponents())
        if (Component* c = static_cast<Component*>(p))
            instance_added.insert(c);

    // Base components = the live component lists of the base GameObjects we pair.
    std::vector<std::pair<GameObject*, GameObject*>> go_pairs;  // (source, instance)
    std::unordered_set<int32_t> base_components;
    for (const auto& e : instance->GetCorrespondingSourceMap())
    {
        GameObject* src_go = Cast<GameObject>(static_cast<Object*>(e.source_object));
        GameObject* inst_go = Cast<GameObject>(static_cast<Object*>(e.instance_object));
        if (src_go == nullptr || inst_go == nullptr)
            continue;
        go_pairs.emplace_back(src_go, inst_go);
        for (ImmediatePtr<Component>& slot : src_go->EditorGetComponentsRef())
            if (Component* c = slot)
                base_components.insert(c->GetInstanceID());
    }

    int changes = 0;

    // ---- 1. detach what the source no longer has --------------------------------
    auto& map = instance->GetCorrespondingSourceMap();
    for (size_t i = 0; i < map.size();)
    {
        Component* inst = Cast<Component>(static_cast<Object*>(map[i].instance_object));
        Component* src = Cast<Component>(static_cast<Object*>(map[i].source_object));
        const int32_t src_id = map[i].source_object.GetInstanceID();
        if (inst == nullptr)
        {
            ++i;  // a GameObject pair
            continue;
        }
        if (src == nullptr)
        {
            // The source does not resolve (e.g. a reloaded scene's reference to a
            // template the variant file gained after it was loaded). Drop the PAIR
            // but keep the component: detaching it would orphan every instance
            // override on it, and step 2/3 re-pair it by adoption.
            map.erase(map.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        const bool stale = chain_removed.count(src_id) != 0 ||
                           (base_components.count(src_id) == 0 && chain_added.count(src_id) == 0);
        if (!stale)
        {
            ++i;
            continue;
        }
        if (GameObject* host = inst->GetParentObject())
            host->EditorRemoveComponent(inst);
        map.erase(map.begin() + static_cast<std::ptrdiff_t>(i));
        ++changes;
    }

    // Is `source` already represented on `instance_host`? Either paired, or an
    // unpaired component of the same type that is not one the instance added itself
    // — the latter is adopted (paired) rather than cloned a second time. That is the
    // state a reloaded scene is in when its pair to a template written after the
    // template's file was loaded came back null.
    auto represented = [&](Component& source, GameObject& instance_host) -> bool {
        if (!instance->FindInstanceForSource(PPtr<Object>(&source)).IsNull())
            return true;
        for (ImmediatePtr<Component>& slot : instance_host.EditorGetComponentsRef())
        {
            Component* c = slot;
            if (c == nullptr || c->GetType() != source.GetType() || instance_added.count(c) != 0)
                continue;
            if (!instance->FindSourceForInstance(PPtr<Object>(c)).IsNull())
                continue;
            PrefabInstance::CorrespondingSourceEntry entry;
            entry.instance_object = PPtr<Object>(c);
            entry.source_object = PPtr<Object>(&source);
            instance->GetCorrespondingSourceMap().push_back(entry);
            return true;
        }
        return false;
    };

    // ---- 2. clone in the chain's additions --------------------------------------
    for (const auto& [template_component, source_host] : structure.added)
    {
        if (instance_removed.count(template_component->GetInstanceID()) != 0)
            continue;
        GameObject* inst_host =
            Cast<GameObject>(static_cast<Object*>(instance->FindInstanceForSource(PPtr<Object>(source_host))));
        if (inst_host == nullptr || represented(*template_component, *inst_host))
            continue;
        if (IntroduceComponentFromSource(instance, *template_component, *inst_host) != nullptr)
            ++changes;
    }

    // ---- 3. re-introduce base components nobody suppresses any more -------------
    for (const auto& [src_go, inst_go] : go_pairs)
    {
        for (ImmediatePtr<Component>& slot : src_go->EditorGetComponentsRef())
        {
            Component* c = slot;
            if (c == nullptr)
                continue;
            const int32_t id = c->GetInstanceID();
            if (chain_removed.count(id) != 0 || instance_removed.count(id) != 0)
                continue;
            if (represented(*c, *inst_go))
                continue;
            if (IntroduceComponentFromSource(instance, *c, *inst_go) != nullptr)
                ++changes;
        }
    }

    if (changes > 0)
        LOG_INFO(ZPrefab, "ReconcileStructure: {} component(s) attached or detached", changes);
    return changes;
}

int PrefabUtilityEditor::MergeFromSource(PrefabInstance* instance)
{
    if (instance == nullptr)
        return 0;

    // Shape first, values second: a component the source just gained has to exist
    // (and be paired) before the value pass below can refresh it.
    ReconcileStructure(instance);

    int merged = 0;

    // Cache modifications keyed by instance target id for O(1) lookup per pair.
    std::unordered_map<int32_t, std::vector<const PropertyModification*>> mods_by_target;
    for (const auto& mod : instance->GetModifications())
    {
        mods_by_target[mod.target.GetInstanceID()].push_back(&mod);
    }

    for (const auto& entry : instance->GetCorrespondingSourceMap())
    {
        Object* instance_obj = static_cast<Object*>(entry.instance_object);
        Object* source_obj = static_cast<Object*>(entry.source_object);
        if (instance_obj == nullptr || source_obj == nullptr)
            continue;

        // Step 1+3: round-trip clone the source bytes into the instance object.
        // We use a value-vector rather than the in-place TransferUtility helper
        // so we can intercept and splice property overrides between write and
        // read.
        //
        // When the source is a Variant these bytes also carry the Variant's own
        // overrides: an instance of a Variant merges to (base + variant), not to the
        // bare base. They are layered onto the BYTES — source_obj is the base
        // template, shared by every instance of that base.
        std::vector<uint8_t> bytes;
        prefab::SerialisedObject src_tree;
        if (!SnapshotEffectiveSource(*source_obj, instance, src_tree, bytes))
        {
            continue;
        }

        // Step 2: splice each instance-side override that targets this object. The
        // splice is the shared byte-level primitive (it also logs why a stale path
        // was skipped) — the tree it needs was built by SnapshotEffectiveSource above.
        auto it = mods_by_target.find(PPtr<Object>(instance_obj).GetInstanceID());
        if (it != mods_by_target.end())
        {
            for (const PropertyModification* mod : it->second)
            {
                PrefabUtility::SpliceModificationIntoBytes(bytes, src_tree, *mod);
            }
        }

        if (ReadBackInto(*instance_obj, bytes))
            ++merged;
    }

    LOG_INFO(ZPrefab,
             "MergeFromSource: merged {} of {} object pairs",
             merged,
             static_cast<int>(instance->GetCorrespondingSourceMap().size()));
    return merged;
}
