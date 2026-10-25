#include "Runtime/Resource/Prefab/PrefabUtility.h"

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Function/Framework/Component/PrefabRefComponent.h"
#include "Runtime/Function/Framework/Component/Rigidbody/RigidbodyComponent.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/Prefab/PrefabAsset.h"

#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "Runtime/BaseClasses/Cast.h"

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
        /// prefab graph (the YAML reader has this directly). Seeding the
        /// Transform -> owning-GameObject map from it lets VisitGameObject
        /// resolve every child Transform's owner, lifting the single-level restriction.
        /// When empty (legacy binary path, which has no flat list), the driver falls back
        /// to root-only discovery and the old Phase-1 multi-level limitation applies.
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

            // Iterative DFS over GameObjects (using `m_PendingGameobjects` as a worklist).
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

GameObject* PrefabUtility::Instantiate(PrefabAsset* asset, bool invoke_post_load)
{
    if (asset == nullptr)
    {
        LOG_ERROR(ZPrefab, "PrefabUtility::Instantiate called with null PrefabAsset");
        return nullptr;
    }

    GameObject* root = asset->GetRootGameObject();
    if (root == nullptr)
    {
        LOG_ERROR(ZPrefab, "PrefabAsset has no root GameObject");
        return nullptr;
    }

    // Phase 1 contract: the caller is responsible for ensuring the asset has been
    // freshly loaded (i.e. not shared with another live instance). The dedicated
    // `InstantiateFromPath` overload makes that explicit by always re-reading the
    // .zasset from disk; calling Instantiate(asset*) directly with a cached
    // PrefabAsset effectively just re-roots the existing tree.
    //
    // True in-memory deep cloning (so multiple independent instances can coexist
    // without re-hitting the disk) is the responsibility of Phase 2/3 — see
    // doc/PrefabSystem_Design.md §3.1 for the design.

    PrefabPostLoadDriver driver;
    driver.Run(root, {}, invoke_post_load);
    return root;
}

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
        if (cached != nullptr && cached->GetType() == TypeOf<PrefabAsset>() &&
            static_cast<PrefabAsset*>(cached)->GetRootGameObject() != nullptr)
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
    if (asset->GetRootGameObject() == nullptr)
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
    std::vector<GameObject*> graph_gameobjects;
    PrefabAsset* asset = LoadPrefabAsset(prefab_path, &graph_gameobjects);
    if (asset == nullptr)
    {
        return nullptr;
    }

    // Seed the post-load driver with the COMPLETE GameObject set so arbitrary-depth
    // child Transforms resolve their owners.
    GameObject* root = asset->GetRootGameObject();
    PrefabPostLoadDriver driver;
    driver.Run(root, graph_gameobjects, invoke_post_load);
    return root;
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
