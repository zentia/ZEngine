#include "Runtime/Resource/Prefab/PrefabUtility.h"

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Function/Framework/Component/PrefabRefComponent.h"
#include "Runtime/Function/Framework/Component/Transform/TransformComponent.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/Prefab/PrefabAsset.h"

#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
    /// Drives PostLoadResource() across the freshly-deserialised prefab subtree using the
    /// canonical scene-graph topology (TransformComponent.m_Children).
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
        /// TransformComponent -> owning-GameObject map from it lets VisitGameObject
        /// resolve every child Transform's owner, lifting the single-level restriction.
        /// When empty (legacy binary path, which has no flat list), the driver falls back
        /// to root-only discovery and the old Phase-1 multi-level limitation applies.
        void Run(GameObject* root, const std::vector<GameObject*>& known_gameobjects = {})
        {
            if (root == nullptr)
            {
                return;
            }

            // 1) Discover all GameObjects reachable from root through Transform children,
            //    so we can map child Transforms back to their owning GameObjects.
            BuildOwnerMap(root, known_gameobjects);

            // 2) DFS, invoking postLoadResource as we go.
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
            // reader). Map each one's TransformComponent -> owner so the DFS below can
            // resolve arbitrary-depth child Transforms. tryGetComponent reads m_Components,
            // which InstantiateFromPath rebuilt before calling us.
            for (GameObject* go : known_gameobjects)
            {
                if (go == nullptr || !m_KnownGameobjects.insert(go).second)
                {
                    continue;
                }
                TransformComponent* tc = go->tryGetComponent(TransformComponent);
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

                TransformComponent* tc = go->tryGetComponent(TransformComponent);
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

            // 2a) Initialise every Component before descending. TransformComponent uses
            //     this to seed its double-buffered transform; other Components capture the
            //     parent GameObject pointer here (Component::m_ParentObject).
            for (ImmediatePtr<Component>& component_ptr : go->getComponents())
            {
                Component* component = component_ptr;
                if (component != nullptr)
                {
                    component->PostLoadResource(go);
                }
            }

            // 2a') Phase 3a — Nested-Prefab expansion.
            //      If `go` carries a PrefabRefComponent that hasn't yet been expanded,
            //      load the referenced PrefabAsset, instantiate it, and reparent the
            //      result under `go`'s TransformComponent. This recurses naturally —
            //      the freshly instantiated nested root will go through its own
            //      `PrefabUtility::Instantiate` call (so any further PrefabRefComponents
            //      inside it expand on their own).
            ExpandNestedPrefabsOn(go);

            // 2b) Recurse into Transform children. Phase 1 keeps things simple: each child
            //     Transform's owning GameObject must already live in the same .prefab
            //     SerializedFile (cross-file children require Phase 3's nested-prefab
            //     resolution). We therefore lazily extend m_TransformToOwner as we walk:
            //     once we know `go` owns its own TransformComponent, we descend into the
            //     children and require that each child Transform belongs to a GameObject
            //     reachable through that child's m_ParentObject back-pointer (which the
            //     postLoadResource above just set on the *parent* component, not the child).
            //
            //     To resolve children we therefore postLoadResource the child GameObject
            //     by searching m_KnownGameobjects for one whose TransformComponent
            //     matches.

            TransformComponent* tc = go->tryGetComponent(TransformComponent);
            if (tc == nullptr)
            {
                return;
            }
            for (size_t i = 0, count = tc->GetChildCount(); i < count; ++i)
            {
                TransformComponent* child_tc = tc->GetChild(i);
                if (child_tc == nullptr)
                {
                    continue;
                }
                auto it = m_TransformToOwner.find(child_tc);
                if (it == m_TransformToOwner.end())
                {
                    // First time we see this child Transform — its owning GameObject was
                    // populated lazily by the prefab loader through ImmediatePtr resolution
                    // (each TransformComponent records its owning GameObject via
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
        std::unordered_map<TransformComponent*, GameObject*> m_TransformToOwner;

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

                PrefabRefComponent* ref = dynamic_cast<PrefabRefComponent*>(comp);
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
                TransformComponent* host_tc = go->tryGetComponent(TransformComponent);
                if (host_tc == nullptr)
                {
                    LOG_WARNING(ZPrefab,
                                "PrefabRefComponent on '{}': host has no TransformComponent — cannot reparent nested",
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

GameObject* PrefabUtility::Instantiate(PrefabAsset* asset)
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
    driver.Run(root);
    return root;
}

GameObject* PrefabUtility::InstantiateFromPath(const std::filesystem::path& prefab_path)
{
    auto&& asset_manager = GET_SYSTEM(AssetManager);

    // .prefab -> YAML object graph; the PrefabAsset header is the entry whose
    // class is PrefabAsset, its m_RootGameObject ImmediatePtr already resolved
    // to the live root GameObject by the graph reader's shared resolver.
    if (prefab_path.extension() == ".prefab")
    {
        std::vector<std::pair<int64_t, Object*>> entries;
        if (!asset_manager->ReadObjectsFromYaml(prefab_path, entries))
        {
            LOG_ERROR(ZPrefab, "Failed to load prefab (yaml) from {}", prefab_path.generic_string());
            return nullptr;
        }

        PrefabAsset* asset = nullptr;
        std::vector<GameObject*> graph_gameobjects;
        graph_gameobjects.reserve(entries.size());
        for (const auto& entry : entries)
        {
            if (entry.second == nullptr)
            {
                continue;
            }
            if (entry.second->GetType() == TypeOf<PrefabAsset>())
            {
                asset = static_cast<PrefabAsset*>(entry.second);
            }
            else if (entry.second->GetType() == TypeOf<GameObject>())
            {
                GameObject* go = static_cast<GameObject*>(entry.second);
                // Rebuild each GameObject's runtime component list from its just-read
                // ImmediatePtr container (binary ReadObject does this inside the asset
                // pipeline; the YAML graph reader doesn't, so do it here).
                go->RebuildRuntimeComponents();
                graph_gameobjects.push_back(go);
            }
        }
        if (asset == nullptr)
        {
            LOG_ERROR(ZPrefab, "Prefab (yaml) {} contained no PrefabAsset header", prefab_path.generic_string());
            return nullptr;
        }

        GameObject* root = asset->GetRootGameObject();
        if (root == nullptr)
        {
            LOG_ERROR(ZPrefab, "Prefab (yaml) {} PrefabAsset has no root GameObject", prefab_path.generic_string());
            return nullptr;
        }

        // Seed the post-load driver with the COMPLETE GameObject set so arbitrary-depth
        // child Transforms resolve their owners (lifts the binary path's single-level
        // Phase-1 restriction for text prefabs).
        PrefabPostLoadDriver driver;
        driver.Run(root, graph_gameobjects);
        return root;
    }

    std::filesystem::path mutable_path = prefab_path;
    PrefabAsset* asset = asset_manager->ReadObject<PrefabAsset>(mutable_path);
    if (asset == nullptr)
    {
        LOG_ERROR(ZPrefab, "Failed to load prefab from {}", prefab_path.generic_string());
        return nullptr;
    }
    return Instantiate(asset);
}

void PrefabUtility::SetInstantiatedRootParent(GameObject* instantiated_root, TransformComponent* parent, bool worldPositionStays)
{
    if (instantiated_root == nullptr)
    {
        return;
    }
    TransformComponent* root_transform = instantiated_root->tryGetComponent(TransformComponent);
    if (root_transform == nullptr)
    {
        LOG_ERROR(ZPrefab, "Instantiated root '{}' has no TransformComponent; cannot reparent", instantiated_root->GetName().c_str());
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
        /// Returns false if any GameObject in the subtree is missing a TransformComponent
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
                if (comp != nullptr)
                {
                    out.push_back(comp);
                }
            }

            // 2) Recurse into Transform children.
            TransformComponent* tc = go->tryGetComponent(TransformComponent);
            if (tc == nullptr)
            {
                // A Prefab whose root has no Transform isn't writable — children are
                // unreachable from a load-side DFS. Caller's contract: pass a live
                // GameObject sourced from a Level (Level::CreateObject guarantees a
                // Transform is present unless the user intentionally stripped one).
                LOG_WARNING(ZPrefab,
                            "PrefabUtility::SaveAsPrefabAsset: GameObject '{}' has no TransformComponent; "
                            "its descendants (if any) cannot be serialised",
                            go->GetName().c_str());
                return true;
            }

            for (size_t i = 0, count = tc->GetChildCount(); i < count; ++i)
            {
                TransformComponent* child_tc = tc->GetChild(i);
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
    MemoryManager::DestroyObject(prefab);

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
