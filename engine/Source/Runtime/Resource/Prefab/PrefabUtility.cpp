#include "Runtime/Resource/Prefab/PrefabUtility.h"

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/Core/Serialize/TransferUtility.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Function/Framework/Component/PrefabRefComponent.h"
#include "Runtime/Function/Framework/Component/Rigidbody/RigidbodyComponent.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/Prefab/PrefabAsset.h"
#include "Runtime/Resource/Prefab/PrefabSubtreeCloner.h"
#include "Runtime/Resource/Prefab/PropertyModification.h"
#include "Runtime/Resource/Prefab/PropertyPathResolver.h"
#include "Runtime/Resource/Prefab/PropertyValueLocator.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "Runtime/BaseClasses/Cast.h"

namespace
{
    /// Post-read preparation of a prefab template GameObject: rebuild the live
    /// component list from the just-read ImmediatePtr container, and bind owner
    /// back-pointers only (no PostLoadResource: the graph is a template, not a live
    /// scene object). Without the owners a child Transform cannot name its
    /// GameObject, so cloning (EditorPrefabCloner), re-saving (SaveAsPrefabAsset's
    /// flattener) and the post-load driver all stop at the root.
    void PrepareTemplateGameObject(GameObject* go)
    {
        go->RebuildRuntimeComponents();
        for (ImmediatePtr<Component>& component : go->getComponents())
        {
            if (Component* c = component)
            {
                c->BindOwnerIfUnbound(go);
            }
        }
    }
}  // namespace

namespace
{
    /// Drives PostLoadResource() across the freshly-deserialised prefab subtree using the
    /// canonical scene-graph topology (Transform.m_Children).
    ///
    /// We can't rely on Component::m_ParentObject being valid until PostLoadResource()
    /// runs — and postLoadResource is exactly what we're trying to invoke. Resolution: do
    /// a one-shot pass to build a {child Transform → owning GameObject} lookup, then DFS
    /// from the root, calling PostLoadResource(go) on every component as we descend.
    class PrefabPostLoadDriver
    {
    public:
        /// `known_gameobjects`, when non-empty, is the COMPLETE set of GameObjects in the
        /// prefab graph. Seeding the Transform -> owning-GameObject map from it lets
        /// VisitGameObject resolve every child Transform's owner and descend the whole
        /// subtree at any depth.
        ///
        /// When it is empty the driver falls back to root-only discovery and
        /// **skips every child subtree** (see the warning in VisitGameObject):
        /// children get no PostLoadResource at all.
        ///
        /// ⚠️ 这条**与文件格式无关**。早先的注释说 "legacy 二进制路径没有 flat
        /// list，所以多层仍受 Phase-1 限制" —— 那是错的：列表不是读出来的，而是
        /// `PrefabUtility::Instantiate` 从 **clone.clones** 拼出来传进来的
        /// （见下面的调用点），`.prefab`（YAML）和 `.zasset`（二进制 SerializedFile）
        /// 走的是同一份代码，行为完全一致。只有"调用方什么都不传"时才会退化成
        /// 单层。已由 ScriptComponentPrefabTest 的 H 组验到三层深
        /// （见 doc/TYPESCRIPT_COMPONENT.md 第 8.5 节）。
        /// `invoke_post_load = false` skips only step 2a (the PostLoadResource call);
        /// the DFS and nested-prefab expansion still run. Used when the tree is being
        /// instantiated as a *definition* whose components will be merged into another
        /// GameObject and post-loaded there — see PrefabUtility::Instantiate.
        void Run(GameObject*                     root,
                 const std::vector<GameObject*>& known_gameobjects = {},
                 bool                            invoke_post_load   = true)
        {
            if (root == nullptr)
            {
                return;
            }

            // 1) Discover all GameObjects reachable from root through Transform children,
            //    so we can map child Transforms back to their owning GameObjects.
            BuildOwnerMap(root, known_gameobjects);

            // 2) DFS, invoking postLoadResource as we go.
            m_InvokePostLoad = invoke_post_load;
            VisitGameObject(root);
        }

    private:
        /// Walks Transform children recursively, recording every GameObject we find. We
        /// look up the owning GO for each Transform by reading Component::m_ParentObject,
        /// but Phase 1's prefab format doesn't serialise that back-pointer — so the only
        /// reliable seed is `root` itself, plus a single dictionary built from the GO's
        /// Component list (which DOES survive serialisation as ImmediatePtr<Component>).
        void BuildOwnerMap(GameObject* root, const std::vector<GameObject*>& known_gameobjects)
        {
            // Preferred path: the caller handed us every GameObject in the graph (YAML
            // reader). Map each one's Transform -> owner so the DFS below can
            // resolve arbitrary-depth child Transforms. tryGetComponent reads m_Components,
            // which InstantiateFromPath rebuilt before calling us.
            for (GameObject* go : known_gameobjects)
            {
                if (go == nullptr || !m_KnownGameobjects.insert(go).second)
                {
                    continue;
                }
                Transform* tc = go->tryGetComponent(Transform);
                if (tc != nullptr)
                {
                    m_TransformToOwner[tc] = go;
                }
            }

            // Fallback: seed from `root` only. NOTE this worklist never enqueues
            // children — so it is not actually a walk over the tree:
            //   * caller passed the complete list  -> `root` is already in
            //     m_KnownGameobjects, the loop `continue`s immediately and this is
            //     dead code (the seed loop above already mapped everything);
            //   * caller passed nothing            -> only `root`'s Transform gets
            //     mapped, and VisitGameObject's missing-owner branch then skips
            //     every child subtree.
            // Deliberately left as-is: it is unreachable for every current caller
            // (Instantiate always supplies the clone's object list), and "silently
            // post-load only the root" is a worse failure than a loud skip+warn.
            std::vector<GameObject*> worklist;
            worklist.push_back(root);

            while (!worklist.empty())
            {
                GameObject* go = worklist.back();
                worklist.pop_back();
                if (go == nullptr || !m_KnownGameobjects.insert(go).second)
                {
                    continue;
                }

                Transform* tc = go->tryGetComponent(Transform);
                if (tc != nullptr)
                {
                    m_TransformToOwner[tc] = go;
                }
                // We can't yet resolve a child Transform's owning GameObject without
                // another back-pointer — defer that to VisitGameObject, which already has
                // the Component → GameObject relation it needs (calling
                // PostLoadResource(go) sets it up).
            }
        }

        void VisitGameObject(GameObject* go)
        {
            if (go == nullptr || !m_Visited.insert(go).second)
            {
                return;
            }

            // 2a) Initialise every Component before descending. Transform uses
            //     this to seed its double-buffered transform; other Components capture the
            //     parent GameObject pointer here (Component::m_ParentObject).
            //
            //     Skipped when the caller asked for no post-load (definition
            //     expansion): those components belong to a throw-away GameObject and
            //     post-loading them allocates resources nobody will ever use or free
            //     (see PrefabUtility::Instantiate's `invoke_post_load`).
            if (m_InvokePostLoad)
            {
                for (ImmediatePtr<Component>& component_ptr : go->getComponents())
                {
                    Component* component = component_ptr;
                    if (component != nullptr)
                    {
                        component->PostLoadResource(go);
                    }
                }
            }

            // 2a') Phase 3a — Nested-Prefab expansion.
            //      If `go` carries a PrefabRefComponent that hasn't yet been expanded,
            //      load the referenced PrefabAsset, instantiate it, and reparent the
            //      result under `go`'s Transform. This recurses naturally —
            //      the freshly instantiated nested root will go through its own
            //      `PrefabUtility::Instantiate` call (so any further PrefabRefComponents
            //      inside it expand on their own).
            ExpandNestedPrefabsOn(go);

            // 2b) Recurse into Transform children. Phase 1 keeps things simple: each child
            //     Transform's owning GameObject must already live in the same .prefab
            //     SerializedFile (cross-file children require Phase 3's nested-prefab
            //     resolution). We therefore lazily extend m_TransformToOwner as we walk:
            //     once we know `go` owns its own Transform, we descend into the
            //     children and require that each child Transform belongs to a GameObject
            //     reachable through that child's m_ParentObject back-pointer (which the
            //     postLoadResource above just set on the *parent* component, not the child).
            //
            //     To resolve children we therefore postLoadResource the child GameObject
            //     by searching m_KnownGameobjects for one whose Transform
            //     matches.

            Transform* tc = go->tryGetComponent(Transform);
            if (tc == nullptr)
            {
                return;
            }
            for (size_t i = 0, count = tc->GetChildCount(); i < count; ++i)
            {
                Transform* child_tc = tc->GetChild(i);
                if (child_tc == nullptr)
                {
                    continue;
                }
                auto it = m_TransformToOwner.find(child_tc);
                if (it == m_TransformToOwner.end())
                {
                    // First time we see this child Transform — its owning GameObject was
                    // populated lazily by the prefab loader through ImmediatePtr resolution
                    // (each Transform records its owning GameObject via
                    // `m_ParentObject`, but only after postLoadResource ran on the *child*
                    // — which is precisely what we're about to do). Fall back to the
                    // child Component's just-set m_ParentObject: postLoadResource sets it
                    // on the parent's Components, not on children, so we must explicitly
                    // call it on the child Transform first to retrieve its owner.
                    //
                    // The child Transform's owning GameObject is unknown to this driver —
                    // skip and warn. Phase 2 will replace this whole driver with a proper
                    // reflection-based owner walker.
                    LOG_WARNING(ZPrefab,
                                "Prefab post-load: child transform has no resolved owning GameObject; "
                                "skipping subtree. Phase 2 will lift this restriction.");
                    continue;
                }
                VisitGameObject(it->second);
            }
        }

        std::unordered_set<GameObject*> m_Visited;
        std::unordered_set<GameObject*> m_KnownGameobjects;
        std::unordered_map<Transform*, GameObject*> m_TransformToOwner;
        bool m_InvokePostLoad {true};

        /// Phase 3a — expand every PrefabRefComponent on `go` exactly once. We
        /// loop over the components looking for one whose IsExpanded() is false;
        /// each found ref is expanded by recursive Instantiate + SetParent. The
        /// component is then marked expanded so re-loading the parent prefab from
        /// disk doesn't re-expand and accumulate phantom subtrees.
        static void ExpandNestedPrefabsOn(GameObject* go)
        {
            if (go == nullptr)
                return;

            // Snapshot the components list because `addComponent` calls during the
            // loop body would invalidate iteration over the live vector. The
            // PrefabRef expansion path itself doesn't touch this GO's component
            // list — but ImmediatePtr<Component>'s operator T*() materialises the
            // pointer, and we want a stable view.
            std::vector<ImmediatePtr<Component>> snapshot = go->getComponents();
            for (ImmediatePtr<Component>& comp_ptr : snapshot)
            {
                Component* comp = comp_ptr;
                if (comp == nullptr)
                    continue;

                PrefabRefComponent* ref = Cast<PrefabRefComponent>(comp);
                if (ref == nullptr || ref->IsExpanded())
                    continue;

                PrefabAsset* nested_asset = static_cast<PrefabAsset*>(ref->GetReferencedPrefab());
                if (nested_asset == nullptr)
                {
                    LOG_WARNING(ZPrefab,
                                "PrefabRefComponent on '{}' has null referenced prefab — skipping expansion",
                                go->GetName().c_str());
                    ref->MarkExpanded();
                    continue;
                }

                // Recursive instantiate. NB: PrefabUtility::Instantiate will run a
                // fresh PrefabPostLoadDriver over the nested subtree, which itself
                // expands further-nested refs. No explicit recursion needed here.
                GameObject* nested_root = PrefabUtility::Instantiate(nested_asset);
                if (nested_root == nullptr)
                {
                    LOG_WARNING(ZPrefab,
                                "PrefabRefComponent on '{}': failed to instantiate referenced prefab",
                                go->GetName().c_str());
                    ref->MarkExpanded();
                    continue;
                }

                // Reparent the nested root under `go`. worldPositionStays=false so
                // the nested root's authored local pose is preserved relative to
                // the install point.
                Transform* host_tc = go->tryGetComponent(Transform);
                if (host_tc == nullptr)
                {
                    LOG_WARNING(ZPrefab,
                                "PrefabRefComponent on '{}': host has no Transform — cannot reparent nested",
                                go->GetName().c_str());
                    ref->MarkExpanded();
                    continue;
                }
                PrefabUtility::SetInstantiatedRootParent(nested_root, host_tc, /*worldPositionStays=*/false);

                ref->MarkExpanded();
                LOG_INFO(ZPrefab,
                         "Expanded nested prefab '{}' under host '{}'",
                         nested_asset->name.c_str(),
                         go->GetName().c_str());
            }
        }
    };
}  // namespace

// =====================================================================================
// Prefab Variant — model A (true inheritance)
// -------------------------------------------------------------------------------------
// A Variant owns NO object tree: it is `m_variant_base` (a PPtr to its base) plus
// `m_variant_overrides` (what this link changes). Instantiating one therefore means:
//
//   1. walk the base chain down to the deepest NON-variant PrefabAsset — that is the
//      only asset in the chain that actually carries GameObjects;
//   2. clone THAT subtree (one clone, shared by the whole chain);
//   3. apply every link's overrides top-down (base -> ... -> variant) onto the clone,
//      so a nearer link wins over a farther one on the same field;
//   4. post-load the clone as usual.
//
// Step 3 needs each override's `target` (an object in the BASE's graph) translated to
// its counterpart in the clone — that is exactly `CloneResult::ptr_map`.
// =====================================================================================
namespace
{
    /// Snapshot `obj` into (type tree, StreamedBinary bytes) — the pair the locator
    /// needs to find a field and overwrite it.
    bool SnapshotObject(Object& obj, prefab::SerialisedObject& out_tree, std::vector<uint8_t>& out_bytes)
    {
        if (!prefab::SerialiseObject(&obj, out_tree))
        {
            return false;
        }
        out_bytes.clear();
        TransferUtility::WriteObjectToVector(obj, out_bytes);
        return true;
    }

    /// Push modified bytes back into the live object. Components get their
    /// OnSerializedFieldsUpdated hook so derived state (e.g. cached matrices) follows.
    // NOTE: non-const ref on purpose — ReadObjectFromVector takes a mutable buffer.
    bool ReadBackInto(Object& obj, std::vector<uint8_t>& bytes)
    {
        if (!TransferUtility::ReadObjectFromVector(obj, bytes))
        {
            return false;
        }
        if (auto* component = Cast<Component>(&obj))
        {
            component->OnSerializedFieldsUpdated();
        }
        return true;
    }

    /// A PPtr leaf occupies one int32 slot holding the target's InstanceID — the same
    /// convention SafeBinaryRead uses to walk past PPtr leaves.
    std::vector<uint8_t> EncodePPtrAsBytes(PPtr<Object> ref)
    {
        int32_t id = ref.GetInstanceID();
        std::vector<uint8_t> out(sizeof(int32_t));
        std::memcpy(out.data(), &id, sizeof(int32_t));
        return out;
    }

    /// Apply ONE PropertyModification to a live object: snapshot -> locate -> splice
    /// -> read back. The override value comes from `object_reference` (PPtr field) if
    /// set, otherwise from `value_bytes` (raw StreamedBinary bytes).
    bool ApplyModification(Object* recipient, const PropertyModification& mod)
    {
        if (recipient == nullptr)
        {
            return false;
        }

        prefab::SerialisedObject tree;
        std::vector<uint8_t> bytes;
        if (!SnapshotObject(*recipient, tree, bytes))
        {
            LOG_WARNING(ZPrefab, "variant override: snapshot failed on '{}'", recipient->name.c_str());
            return false;
        }

        std::vector<prefab::PathToken> tokens;
        if (!prefab::TokenizePropertyPath(mod.property_path, tokens))
        {
            LOG_WARNING(ZPrefab, "variant override: malformed property_path '{}'", mod.property_path.c_str());
            return false;
        }

        prefab::PropertyLocation loc;
        if (!prefab::LocatePropertyInStream(tree.type_tree, bytes, tokens, loc) || !loc.valid())
        {
            LOG_WARNING(ZPrefab, "variant override: path '{}' not found on '{}'",
                        mod.property_path.c_str(), recipient->name.c_str());
            return false;
        }

        std::vector<uint8_t> value;
        if (!mod.object_reference.IsNull())
        {
            value = EncodePPtrAsBytes(mod.object_reference);
        }
        else
        {
            value = mod.value_bytes;
        }

        if (value.size() != static_cast<size_t>(loc.byte_size))
        {
            LOG_WARNING(ZPrefab,
                        "variant override: value size {} != field size {} for '{}' on '{}'",
                        static_cast<int>(value.size()), static_cast<int>(loc.byte_size),
                        mod.property_path.c_str(), recipient->name.c_str());
            return false;
        }

        if (!prefab::OverwriteScalarBytes(bytes, loc, value.data(), value.size()))
        {
            return false;
        }
        return ReadBackInto(*recipient, bytes);
    }

}  // namespace

bool PrefabUtility::ResolveVariantChain(PrefabAsset* asset, std::vector<PrefabAsset*>& out_chain)
{
    out_chain.clear();
    PrefabAsset* node = asset;
    constexpr int kMaxDepth = 64;
    while (node != nullptr)
    {
        if (static_cast<int>(out_chain.size()) >= kMaxDepth)
        {
            LOG_ERROR(ZPrefab, "variant chain longer than {} links — cycle?", kMaxDepth);
            return false;
        }
        out_chain.push_back(node);
        if (!node->IsVariant())
        {
            break;
        }
        PrefabAsset* base = static_cast<PrefabAsset*>(node->GetVariantBase());
        if (base == nullptr)
        {
            LOG_ERROR(ZPrefab, "variant chain broken — '{}' is marked variant but its base PPtr is null",
                      node->name.c_str());
            return false;
        }
        node = base;
    }
    std::reverse(out_chain.begin(), out_chain.end());  // deepest first
    return true;
}

int PrefabUtility::ApplyVariantOverrides(const std::vector<PrefabAsset*>&             chain,
                                         const std::unordered_map<Object*, Object*>&  source_to_clone)
{
    int applied = 0;
    for (PrefabAsset* layer : chain)
    {
        for (const PropertyModification& mod : layer->GetVariantOverrides())
        {
            Object* source = static_cast<Object*>(mod.target);
            if (source == nullptr)
            {
                continue;
            }
            auto it = source_to_clone.find(source);
            if (it == source_to_clone.end() || it->second == nullptr)
            {
                // The override names an object the base no longer has (base was
                // edited since). Not fatal — skip it rather than fail the whole
                // instantiation.
                LOG_WARNING(ZPrefab, "variant override on '{}': target object is not in the base subtree — skipped",
                            layer->name.c_str());
                continue;
            }
            if (ApplyModification(it->second, mod))
            {
                ++applied;
            }
        }
    }
    return applied;
}

GameObject* PrefabUtility::Instantiate(PrefabAsset* asset, bool invoke_post_load)
{
    if (asset == nullptr)
    {
        LOG_ERROR(ZPrefab, "PrefabUtility::Instantiate called with null PrefabAsset");
        return nullptr;
    }

    // A Variant carries no object tree of its own — resolve the chain and clone the
    // deepest non-variant base instead. For a plain prefab the chain is one element
    // long and this is exactly the old behaviour.
    std::vector<PrefabAsset*> chain;
    if (!ResolveVariantChain(asset, chain))
    {
        return nullptr;
    }

    PrefabAsset* base_asset = chain.front();
    GameObject* root = base_asset->GetRootGameObject();
    if (root == nullptr)
    {
        LOG_ERROR(ZPrefab, "PrefabAsset '{}' (base of the variant chain) has no root GameObject",
                  base_asset->name.c_str());
        return nullptr;
    }

    // `asset` is the TEMPLATE: a prefab file's graph is loaded once and cached under
    // path-derived instance IDs (LoadPrefabAsset), so handing out its objects -- what
    // this used to do, relying on "every load re-reads the file" -- gave every caller
    // the same GameObject / components: two drops of one prefab shared one
    // Transform, and every scene object expanding the same m_DefinitionUrl got the
    // same component instances. Clone the template instead; it stays untouched.
    if (root->getComponents().empty())
    {
        // Loaded by a lazy PPtr deref rather than LoadPrefabAsset.
        PrepareTemplateGameObject(root);
    }
    prefab_clone::CloneResult clone = prefab_clone::CloneSubtree(root);
    if (!clone.valid())
    {
        LOG_ERROR(ZPrefab, "PrefabUtility::Instantiate: cloning '{}' failed", root->GetName().c_str());
        return nullptr;
    }

    std::vector<GameObject*> clone_gameobjects;
    for (Object* object : clone.clones)
    {
        if (GameObject* go = Cast<GameObject>(object))
        {
            clone_gameobjects.push_back(go);
            // Owners for the post-load driver's child resolution; with
            // invoke_post_load the driver's PostLoadResource sets them again.
            for (ImmediatePtr<Component>& component : go->getComponents())
            {
                if (Component* c = component)
                {
                    c->BindOwnerIfUnbound(go);
                }
            }
        }
    }

    // Variant overrides go on BEFORE the post-load pass: PostLoadResource is what
    // binds TypeScriptComponents and seeds Transform's double buffer, and it must
    // see the overridden values, not the base's.
    const int applied = chain.size() > 1 ? ApplyVariantOverrides(chain, clone.ptr_map) : 0;
    if (applied > 0)
    {
        LOG_INFO(ZPrefab, "Instantiate: applied {} variant override(s) from a {}-link chain onto '{}'",
                 applied, static_cast<int>(chain.size()), root->GetName().c_str());
    }

    PrefabPostLoadDriver driver;
    driver.Run(clone.clone_root, clone_gameobjects, invoke_post_load);
    return clone.clone_root;
}


PrefabAsset* PrefabUtility::LoadPrefabAsset(const std::filesystem::path& prefab_path, std::vector<GameObject*>* out_gameobjects)
{
    auto&& asset_manager = GET_SYSTEM(AssetManager);
    if (asset_manager == nullptr)
    {
        return nullptr;
    }
    if (out_gameobjects != nullptr)
    {
        out_gameobjects->clear();
    }

    const bool is_yaml = AssetManager::IsYamlAuthoringAssetPath(prefab_path);
    ObjectManager* object_manager = GET_SYSTEM(ObjectManager);

    // Graphs live under path-derived instance IDs (binary and, since 2026-10-26,
    // YAML too): reading the file a second time would re-Produce objects under the
    // same IDs and silently re-point every PPtr into the second copy. Reuse the
    // hydrated graph instead.
    {
        const int32_t header_id = asset_manager->GetInstanceIDFromPathAndFileID(prefab_path, 1);
        Object* cached = header_id != 0 ? object_manager->IDToPointer(header_id) : nullptr;
        // A Variant legitimately has no root (the tree lives in its base), so
        // "root != nullptr || IsVariant" is what makes a hydrated graph reusable.
        // Without the IsVariant() half, every load of a variant file would
        // re-Produce it under the same path-derived IDs.
        if (cached != nullptr && cached->GetType() == TypeOf<PrefabAsset>() &&
            (static_cast<PrefabAsset*>(cached)->GetRootGameObject() != nullptr ||
             static_cast<PrefabAsset*>(cached)->IsVariant()))
        {
            // SaveAsPrefabAsset numbers the objects 1..N without gaps.
            for (int64_t file_id = 2;; ++file_id)
            {
                Object* object = object_manager->IDToPointer(asset_manager->GetInstanceIDFromPathAndFileID(prefab_path, file_id));
                if (object == nullptr)
                {
                    break;
                }
                if (object->GetType() == TypeOf<GameObject>())
                {
                    GameObject* go = static_cast<GameObject*>(object);
                    // The graph may have been hydrated by a lazy PPtr deref
                    // (AssetManager::ReadObject) rather than by this function, in
                    // which case the post-read preparation below never ran. Only
                    // when the live list is still empty: a prepared graph may have
                    // been edited since (Apply) and must not be reset.
                    if (go->getComponents().empty())
                    {
                        PrepareTemplateGameObject(go);
                    }
                    if (out_gameobjects != nullptr)
                    {
                        out_gameobjects->push_back(go);
                    }
                }
            }
            return static_cast<PrefabAsset*>(cached);
        }
    }

    std::vector<std::pair<int64_t, Object*>> entries;
    const bool read_ok = is_yaml ? asset_manager->ReadObjectsFromYaml(prefab_path, entries)
                                 : asset_manager->ReadObjectsFromBinary(prefab_path, entries);
    if (!read_ok)
    {
        LOG_ERROR(ZPrefab, "Failed to load prefab from {}", prefab_path.generic_string());
        return nullptr;
    }

    // The header is the PrefabAsset at fileID 1 (any PrefabAsset if a hand-edited
    // file renumbered it); its m_RootGameObject resolved through the graph reader's
    // shared resolver. Every GameObject gets its runtime component list rebuilt from
    // the just-read ImmediatePtr container -- neither graph reader does that, and
    // getComponents() / tryGetComponent() are empty until it happens (Level::Load
    // does the same for scenes).
    PrefabAsset* asset = nullptr;
    for (const auto& entry : entries)
    {
        Object* object = entry.second;
        if (object == nullptr)
        {
            continue;
        }
        if (object->GetType() == TypeOf<PrefabAsset>())
        {
            if (asset == nullptr || entry.first == 1)
            {
                asset = static_cast<PrefabAsset*>(object);
            }
        }
        else if (object->GetType() == TypeOf<GameObject>())
        {
            GameObject* go = static_cast<GameObject*>(object);
            PrepareTemplateGameObject(go);
            if (out_gameobjects != nullptr)
            {
                out_gameobjects->push_back(go);
            }
        }
    }

    if (asset == nullptr)
    {
        LOG_ERROR(ZPrefab, "Prefab {} contained no PrefabAsset header", prefab_path.generic_string());
        return nullptr;
    }
    // A Variant has no root of its own by design — it is a base PPtr plus overrides,
    // and the object tree lives in the base. Rejecting those here would make every
    // .prefab written by CreateVariant unloadable.
    if (asset->GetRootGameObject() == nullptr && !asset->IsVariant())
    {
        LOG_ERROR(ZPrefab, "Prefab {} PrefabAsset has no root GameObject", prefab_path.generic_string());
        return nullptr;
    }
    return asset;
}

GameObject* PrefabUtility::InstantiateFromPath(const std::filesystem::path& prefab_path, bool invoke_post_load)
{
    // Whole-graph load for both formats. This used to read only the PrefabAsset
    // header for binary .zasset prefabs (AssetManager::ReadObject<PrefabAsset>),
    // which never produces the root GameObject the header points at -- so binary
    // prefabs always failed with "PrefabAsset has no root GameObject".
    // The graph is cached after the first load; Instantiate clones it.
    PrefabAsset* asset = LoadPrefabAsset(prefab_path);
    if (asset == nullptr)
    {
        return nullptr;
    }
    return Instantiate(asset, invoke_post_load);
}

void PrefabUtility::SetInstantiatedRootParent(GameObject* instantiated_root, Transform* parent, bool worldPositionStays)
{
    if (instantiated_root == nullptr)
    {
        return;
    }
    Transform* root_transform = instantiated_root->tryGetComponent(Transform);
    if (root_transform == nullptr)
    {
        LOG_ERROR(ZPrefab, "Instantiated root '{}' has no Transform; cannot reparent", instantiated_root->GetName().c_str());
        return;
    }
    root_transform->SetParent(parent, worldPositionStays);
}

namespace
{
    /// DFS the live GameObject subtree rooted at `root`, producing a flat object list
    /// in the exact order PrefabUtility wants to write it to a SerializedFile:
    ///
    ///   [0] PrefabAsset  (caller pre-pends this — fileID 1)
    ///   [1] root GameObject                                            (fileID 2)
    ///   [2..] root's Components, in declaration order                  (fileID 3..)
    ///   [..]  child GameObject 0                                       (fileID N)
    ///   [..]  child 0's Components                                     (...)
    ///   [..]  child GameObject 1
    ///   ...
    ///
    /// Visiting a child Transform requires going Transform → owning GameObject. We
    /// rely on `Component::m_ParentObject`, which `Level::createObject` →
    /// `GameObject::load` → `Component::PostLoadResource(this)` already populates for
    /// every GameObject that lives in a Level. Calls from outside that pipeline are
    /// rejected (we'd otherwise produce a dangling-pointer-only Prefab).
    class PrefabSubtreeFlattener
    {
    public:
        /// Returns false if any GameObject in the subtree is missing a Transform
        /// or has a child Transform whose owning GameObject can't be resolved.
        bool Run(GameObject* root, std::vector<Object*>& out_objects)
        {
            if (root == nullptr)
            {
                return false;
            }
            return Visit(root, out_objects);
        }

    private:
        /// Append Objects owned by a Component (rather than by the GameObject)
        /// to the writer's object list, so their fileID references resolve.
        static void CollectEmbeddedObjects(const Component* comp, std::vector<Object*>& out)
        {
            const RigidBodyComponent* rigidbody = Cast<RigidBodyComponent>(comp);
            if (rigidbody == nullptr)
            {
                return;
            }

            for (const ImmediatePtr<RigidBodyShape>& shape : rigidbody->GetRigidbodyRes().m_Shapes)
            {
                RigidBodyShape* shape_object = shape;
                if (shape_object != nullptr)
                {
                    out.push_back(shape_object);
                }
            }
        }

        bool Visit(GameObject* go, std::vector<Object*>& out)
        {
            if (go == nullptr || !m_Visited.insert(go).second)
            {
                // Cycle / re-entry — skip silently. A well-formed Transform graph never
                // re-enters; if it does, the second visit would just duplicate fileIDs.
                return true;
            }

            out.push_back(go);

            // 1) Append every Component of this GO. The Prefab's binary layout reads
            //    Components through ImmediatePtr<Component> entries on the GameObject's
            //    m_Components vector — those ImmediatePtrs serialise as fileIDs into
            //    THIS file, so each Component must end up with its own slot.
            for (ImmediatePtr<Component>& comp_ptr : go->getComponents())
            {
                Component* comp = comp_ptr;
                if (comp == nullptr)
                {
                    continue;
                }

                out.push_back(comp);

                // Some Components embed Objects of their own. Those are
                // serialized as fileID references too, so they need slots in
                // this file -- without them the reference resolves to null on
                // read (a wall would load with a rigid body but no shape).
                CollectEmbeddedObjects(comp, out);
            }

            // 2) Recurse into Transform children.
            Transform* tc = go->tryGetComponent(Transform);
            if (tc == nullptr)
            {
                // A Prefab whose root has no Transform isn't writable — children are
                // unreachable from a load-side DFS. Caller's contract: pass a live
                // GameObject sourced from a Level (Level::CreateObject guarantees a
                // Transform is present unless the user intentionally stripped one).
                LOG_WARNING(ZPrefab,
                            "PrefabUtility::SaveAsPrefabAsset: GameObject '{}' has no Transform; "
                            "its descendants (if any) cannot be serialised",
                            go->GetName().c_str());
                return true;
            }

            for (size_t i = 0, count = tc->GetChildCount(); i < count; ++i)
            {
                Transform* child_tc = tc->GetChild(i);
                if (child_tc == nullptr)
                {
                    continue;
                }
                GameObject* child_go = child_tc->GetParentObject();
                if (child_go == nullptr)
                {
                    LOG_ERROR(ZPrefab,
                              "PrefabUtility::SaveAsPrefabAsset: child Transform under '{}' has no owning "
                              "GameObject (Component::m_parent_object is null). Refusing to write a partial "
                              "prefab — please make sure the source GameObject is alive in a Level.",
                              go->GetName().c_str());
                    return false;
                }
                if (!Visit(child_go, out))
                {
                    return false;
                }
            }
            return true;
        }

        std::unordered_set<GameObject*> m_Visited;
    };
}  // namespace

bool PrefabUtility::SaveAsPrefabAsset(GameObject* root, const std::filesystem::path& prefab_path)
{
    if (root == nullptr)
    {
        LOG_ERROR(ZPrefab, "SaveAsPrefabAsset: root GameObject is null");
        return false;
    }
    if (prefab_path.empty())
    {
        LOG_ERROR(ZPrefab, "SaveAsPrefabAsset: prefab_path is empty");
        return false;
    }

    // 1) Flatten the live GameObject subtree into a writer-ready object list.
    //    Slot 0 is reserved for the PrefabAsset header (fileID=1) — see comment
    //    on PrefabSubtreeFlattener.
    std::vector<Object*> objects;
    objects.reserve(16);
    objects.push_back(nullptr);  // placeholder for the PrefabAsset slot

    PrefabSubtreeFlattener flattener;
    if (!flattener.Run(root, objects))
    {
        return false;
    }

    if (objects.size() < 2u)
    {
        LOG_ERROR(ZPrefab, "SaveAsPrefabAsset: produced an empty subtree for '{}'", root->GetName().c_str());
        return false;
    }

    // 2) Allocate a fresh PrefabAsset header. ObjectManager::Produce is the canonical
    //    factory for Object subclasses participating in serialisation (it sets up the
    //    InstanceID, registers with the global object table, and runs the type's
    //    REGISTER_CLASS hooks). Anything allocated outside that path won't write
    //    properly.
    PrefabAsset* prefab = static_cast<PrefabAsset*>(GET_SYSTEM(ObjectManager)->Produce(TypeOf<PrefabAsset>(), 0));
    if (prefab == nullptr)
    {
        LOG_ERROR(ZPrefab, "SaveAsPrefabAsset: failed to allocate PrefabAsset for '{}'", prefab_path.generic_string());
        return false;
    }

    // PrefabAsset.m_RootGameObject is an ImmediatePtr<GameObject>; the writer
    // emits it as a fileID reference into the same SerializedFile. Because we
    // place `root` at position 1 in `objects` (fileID = position+1 = 2), this
    // ImmediatePtr will resolve to fileID=2 on read-back. No extra wiring needed.
    prefab->SetRootGameObject(root);
    objects[0] = prefab;

    // 3) Hand the ordered object list to AssetManager. Passing nullptr for
    //    identifiers makes the writer assign sequential localIdentifierInFile
    //    starting at 1 — exactly the layout we want.
    bool ok = false;
    {
        const std::filesystem::path parent_dir = prefab_path.parent_path();
        if (!parent_dir.empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(parent_dir, ec);
            // Non-fatal: WriteObjectsToDiskThreadSafe will surface the actual error
            // if the directory still doesn't exist.
        }

        // Authoring prefabs (.prefab) persist as a human-readable YAML object
        // graph; legacy binary prefabs (.zasset) keep the SerializedFile path.
        // Both share the exact same ordered object list and reference model
        // (local fileID for in-file refs, GUID externals for imported assets).
        if (prefab_path.extension() == ".prefab")
        {
            ok = GET_SYSTEM(AssetManager)->WriteObjectsToYaml(prefab_path, objects.data(), /*identifiers=*/nullptr, objects.size());
        }
        else
        {
            ok = GET_SYSTEM(AssetManager)->WriteObjectsToDiskThreadSafe(prefab_path, objects.data(), /*identifiers=*/nullptr, objects.size());
        }
    }

    // 4) Tear down our transient PrefabAsset header. The live `root` GameObject and
    //    its Components belong to the caller's Level — we MUST NOT destroy them.
    //
    //    ⚠️ 必须走 ObjectManager::Destroy（= delete），不能用 MemoryManager::DestroyObject：
    //    Produce() 内部是 `new T()`（见 ObjectDefines.h 的 ProduceHelper），而 Apple 上
    //    全局 operator new/delete **故意不接管** mimalloc（MemoryManager.cpp 的
    //    `#if !defined(__APPLE__)` 分支），把 new 出来的指针喂给 mi_free 会在
    //    mi_usable_size → mi_checked_ptr_segment 里 EXC_BAD_ACCESS / SIGBUS。
    //    非 Apple 上 new 就是 mimalloc，所以这条错配在 Windows/Linux 上一直是隐形的。
    GET_SYSTEM(ObjectManager)->Destroy(prefab);

    if (!ok)
    {
        LOG_ERROR(ZPrefab, "SaveAsPrefabAsset: AssetManager refused to write '{}'", prefab_path.generic_string());
        return false;
    }

    LOG_INFO(ZPrefab,
             "Saved prefab '{}' ({} objects) from GameObject '{}'",
             prefab_path.generic_string(),
             static_cast<int>(objects.size()),
             root->GetName().c_str());
    return true;
}

// =====================================================================================
// Prefab Variant — authoring (create / save)
// -------------------------------------------------------------------------------------
// A Variant file is a one-object prefab: the PrefabAsset header, with
// m_RootGameObject left null on purpose and m_VariantBase pointing at the base's
// file. Loading one therefore needs LoadPrefabAsset to accept "no root but
// IsVariant()" (see its comment), and instantiating one needs the chain walk.
//
// Both of these live on the Runtime side rather than in the Editor module so a
// build tool can generate variants too — and, more importantly, because the
// Editor's PrefabInstance is kTypeIsEditorOnly: anything that only exists there
// never reaches the player.
// =====================================================================================

PrefabAsset* PrefabUtility::CreateVariantAsset(PrefabAsset* base, const eastl::string& variant_name)
{
    if (base == nullptr)
    {
        LOG_ERROR(ZPrefab, "CreateVariantAsset: base asset is null");
        return nullptr;
    }

    ObjectManager* object_manager = GET_SYSTEM(ObjectManager);
    if (object_manager == nullptr)
    {
        LOG_ERROR(ZPrefab, "CreateVariantAsset: no ObjectManager");
        return nullptr;
    }

    Object* raw = object_manager->Produce(TypeOf<PrefabAsset>(), 0);
    if (raw == nullptr)
    {
        LOG_ERROR(ZPrefab, "CreateVariantAsset: failed to allocate PrefabAsset");
        return nullptr;
    }

    PrefabAsset* variant = static_cast<PrefabAsset*>(raw);
    variant->name = variant_name;
    variant->SetVariantBase(base);

    LOG_INFO(ZPrefab, "Created prefab variant '{}' (base='{}')",
             variant_name.c_str(), base->name.c_str());
    return variant;
}

bool PrefabUtility::SaveVariantAsset(PrefabAsset* variant, const std::filesystem::path& variant_path)
{
    if (variant == nullptr)
    {
        LOG_ERROR(ZPrefab, "SaveVariantAsset: variant asset is null");
        return false;
    }
    if (variant_path.empty())
    {
        LOG_ERROR(ZPrefab, "SaveVariantAsset: variant_path is empty");
        return false;
    }
    if (!variant->IsVariant())
    {
        LOG_ERROR(ZPrefab, "SaveVariantAsset: '{}' has no base — it is a plain prefab, not a Variant",
                  variant->name.c_str());
        return false;
    }

    AssetManager* asset_manager = GET_SYSTEM(AssetManager);
    if (asset_manager == nullptr)
    {
        LOG_ERROR(ZPrefab, "SaveVariantAsset: no AssetManager");
        return false;
    }

    const std::filesystem::path parent_dir = variant_path.parent_path();
    if (!parent_dir.empty())
    {
        std::error_code ec;
        std::filesystem::create_directories(parent_dir, ec);
    }

    // Exactly one object: the header. Slot 0 is the PrefabAsset (fileID = 1), which
    // is also what LoadPrefabAsset looks for.
    Object* objects[1] = { variant };
    const bool ok = (variant_path.extension() == ".prefab")
        ? asset_manager->WriteObjectsToYaml(variant_path, objects, /*identifiers=*/nullptr, 1u)
        : asset_manager->WriteObjectsToDiskThreadSafe(variant_path, objects, /*identifiers=*/nullptr, 1u);

    if (!ok)
    {
        LOG_ERROR(ZPrefab, "SaveVariantAsset: AssetManager refused to write '{}'",
                  variant_path.generic_string());
        return false;
    }

    LOG_INFO(ZPrefab, "Saved prefab variant '{}' ({} override(s), base='{}')",
             variant_path.generic_string(),
             static_cast<int>(variant->GetVariantOverrides().size()),
             static_cast<PrefabAsset*>(variant->GetVariantBase())->name.c_str());
    return true;
}
