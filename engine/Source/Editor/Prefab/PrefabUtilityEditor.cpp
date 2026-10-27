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

#include <cstring>
#include <unordered_map>
#include "Runtime/BaseClasses/Cast.h"
#include "Runtime/Function/Framework/Component/Component.h"

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
            LOG_WARNING(ZPrefab, "SpliceFieldValueInto: path '{}' not found on recipient", path.c_str());
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
                LOG_WARNING(ZPrefab, "SpliceFieldValueInto: path '{}' not found on donor", path.c_str());
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

    /// Bridge between the public `std::vector<uint8_t>` API surface and the storage
    /// type on PropertyModification (eastl::string — chosen because SerializeTraits
    /// has full coverage for it whereas std::vector<uint8_t> would need a new
    /// uint8_t specialization). The string can hold arbitrary bytes including 0x00,
    /// so this is a faithful round-trip.
    /// Snapshot the value an instance object should REVERT / MERGE to, plus its type
    /// tree, WITHOUT mutating `source_obj`.
    ///
    /// `source_obj` is a base template object — shared by every instance of that base —
    /// so layering the Variant's overrides onto it in place would corrupt all of them.
    /// `PrefabUtility::SnapshotWithOverrides` layers them onto the BYTES instead.
    ///
    /// For a plain prefab the variant layer is empty and this is just a snapshot.
    bool SnapshotEffectiveSource(Object& source_obj, const PrefabInstance* instance,
                                 prefab::SerialisedObject& out_tree, std::vector<uint8_t>& out_bytes)
    {
        std::vector<PropertyModification> layer;
        if (instance != nullptr)
        {
            if (const PrefabAsset* variant = static_cast<const PrefabAsset*>(instance->GetSourcePrefab()))
            {
                if (variant->IsVariant())
                {
                    layer = variant->GetVariantOverrides();
                }
            }
        }
        return PrefabUtility::SnapshotWithOverrides(source_obj, layer, out_tree, out_bytes);
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
        LOG_WARNING(ZPrefab, "RecordPropertyOverride: path '{}' does not exist on target", path.c_str());
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
        LOG_WARNING(ZPrefab, "RecordCurrentValueAsOverride: path '{}' not found", path.c_str());
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
    GameObject* src_host = src_comp->GetParentObject();
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

    // 3) Clone the source Component via the round-trip helper. Since a single
    //    Component has no nested PPtrs in the prefab subtree (its references go
    //    OUT to assets, materials, etc., not into the prefab), the generic
    //    round-trip is sufficient here — we don't need the full structural
    //    cloner used for whole-subtree cases.
    Object* clone_obj = TransferUtility::CloneObjectViaSerialization(*src_comp);
    if (clone_obj == nullptr)
    {
        LOG_ERROR(ZPrefab, "UnsuppressSourceComponent: CloneObjectViaSerialization returned null");
        return false;
    }
    Component* clone_comp = Cast<Component>(clone_obj);
    if (clone_comp == nullptr)
    {
        LOG_ERROR(ZPrefab, "UnsuppressSourceComponent: clone is not a Component");
        return false;
    }

    // 4) Bind the clone's parent + attach to the instance host.
    clone_comp->PostLoadResource(instance_host);
    instance_host->addComponent(clone_comp);

    // 5) Pair the clone with its source in the corresponding-source map so
    //    subsequent property overrides on the un-suppressed component still
    //    resolve apply/revert correctly.
    PrefabInstance::CorrespondingSourceEntry entry;
    entry.instance_object = PPtr<Object>(clone_comp);
    entry.source_object = PPtr<Object>(src_comp);
    instance->GetCorrespondingSourceMap().push_back(entry);

    // 6) Drop from m_RemovedComponents.
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

    // Structural changes come first, same as Runtime's Instantiate: a component the
    // variant removes must be gone before the post-load pass walks the tree, and one
    // it adds must already be attached to its host by then.
    const int structural = PrefabUtility::ApplyVariantStructuralChanges(chain, source_to_clone);
    const int applied = structural + PrefabUtility::ApplyVariantOverrides(chain, source_to_clone);

    // Re-point m_SourcePrefab at the variant we were called with so future
    // Apply pushes overrides into `variant`, not into `deepest`.
    instance->SetSourcePrefab(PPtr<PrefabAsset>(variant));

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

int PrefabUtilityEditor::ApplyAllToVariant(PrefabInstance* instance)
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

    int applied = 0;
    for (const PropertyModification& mod : instance->GetModifications())
    {
        if (mod.object_reference.IsNull() && mod.value_bytes.empty())
        {
            LOG_WARNING(ZPrefab, "ApplyAllToVariant: '{}' has neither a value nor a reference — skipped",
                        mod.property_path.c_str());
            continue;
        }

        // The override must name the object in the BASE's graph — that is what both
        // Runtime's ptr_map and the editor's corresponding_source are keyed by.
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
    // the INSTANCE and `m_removed_components` already names the base's components,
    // so both have to be re-expressed against the base graph before they can be
    // stored on the variant — which is the graph every link of the chain targets.
    auto& variant_added = variant->GetVariantAddedComponents();
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
        Object* clone_object = TransferUtility::CloneObjectViaSerialization(*added);
        Component* clone_component = (clone_object != nullptr) ? Cast<Component>(clone_object) : nullptr;
        if (clone_component == nullptr)
        {
            LOG_WARNING(ZPrefab, "ApplyAllToVariant: cloning the added component failed — skipped");
            continue;
        }
        // It is about to become an object of the variant file, so it needs an
        // identity of its own.
        if (ObjectManager* object_manager = GET_SYSTEM(ObjectManager))
        {
            object_manager->AllocateAndAssignInstanceID(clone_component);
        }

        VariantComponentAddition addition;
        addition.host      = PPtr<GameObject>(base_host);
        addition.component = clone_component;
        variant_added.push_back(addition);
        ++applied;
    }

    auto& variant_removed = variant->GetVariantRemovedComponents();
    for (const PPtr<Component>& entry : instance->GetRemovedComponents())
    {
        if (entry.IsNull())
        {
            continue;
        }
        const int32_t id = entry.GetInstanceID();
        bool already = false;
        for (const PPtr<Component>& existing : variant_removed)
        {
            if (existing.GetInstanceID() == id)
            {
                already = true;
                break;
            }
        }
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
    if (asset_manager == nullptr ||
        !asset_manager->TryGetIdentityForInstance(variant->GetInstanceID(), path, header_file_id) ||
        path.empty())
    {
        LOG_ERROR(ZPrefab, "ApplyAllToVariant: the variant asset has no file identity; "
                           "{} override(s) left unsaved", applied);
        return 0;
    }

    if (!PrefabUtility::SaveVariantAsset(variant, path))
    {
        LOG_ERROR(ZPrefab, "ApplyAllToVariant: writing '{}' failed; {} override(s) left unsaved",
                  path.generic_string(), applied);
        return 0;
    }

    // Unity: after Apply the instance no longer overrides anything — the variant now
    // carries those values and that structure itself.
    instance->ClearModifications();
    instance->GetAddedComponents().clear();
    instance->GetRemovedComponents().clear();

    LOG_INFO(ZPrefab, "ApplyAllToVariant: {} change(s) recorded on '{}' "
                      "({} override(s), {} added / {} removed component(s))",
             applied, path.generic_string(),
             static_cast<int>(variant->GetVariantOverrides().size()),
             static_cast<int>(variant->GetVariantAddedComponents().size()),
             static_cast<int>(variant->GetVariantRemovedComponents().size()));
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

int PrefabUtilityEditor::MergeFromSource(PrefabInstance* instance)
{
    if (instance == nullptr)
        return 0;

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
