#include "Editor/Prefab/EditorPrefabCloner.h"

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/BaseClasses/Object.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Serialize/TransferUtility.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Function/Framework/Component/Transform/TransformComponent.h"

#include <unordered_set>
#include <vector>

namespace prefab_editor
{

    namespace
    {

        /// Round-trips `source` into `clone` via WriteObjectToVector + ReadObjectFromVector.
        /// This faithfully duplicates plain data fields (scalars, strings, std::vector of
        /// trivially-serialisable types) but DOES NOT carry over pointer fields:
        ///   - PPtr<T>::Transfer is a no-op (see PPtr.h:97-99) → clone PPtrs come out as 0.
        ///   - ImmediatePtr<T>::Transfer only writes file-id metadata → clone m_Ptr stays
        ///     null/whatever Read leaves it as.
        /// Caller is expected to repair both kinds of pointer afterwards using ptr_map.
        bool CopyPlainFields(const Object& source, Object& clone)
        {
            std::vector<uint8_t> buffer;
            TransferUtility::WriteObjectToVector(source, buffer);
            if (buffer.empty())
            {
                // Empty bytes is OK for objects with no transferable fields, e.g. a vanilla
                // Object subclass that only inherits Object::Transfer (which is itself a
                // no-op). The clone is already correctly default-constructed so we can
                // claim success.
                return true;
            }
            return TransferUtility::ReadObjectFromVector(clone, buffer);
        }

        /// BFS the prefab subtree starting at `source_root`. Visits every GameObject
        /// reachable via Transform.m_Children, plus every Component owned by each
        /// GameObject. Returns the visit order; the caller uses it to allocate clones
        /// and pair them up.
        struct SourceWalkResult
        {
            std::vector<GameObject*> game_objects;  // root first, then DFS over children
            std::vector<Component*> components;     // index-aligned with `component_owners`
            std::vector<GameObject*> component_owners;
        };

        SourceWalkResult WalkSourceTree(GameObject* source_root)
        {
            SourceWalkResult result;
            std::unordered_set<GameObject*> visited_gos;
            std::vector<GameObject*> worklist;
            worklist.push_back(source_root);

            while (!worklist.empty())
            {
                GameObject* go = worklist.back();
                worklist.pop_back();
                if (go == nullptr || !visited_gos.insert(go).second)
                {
                    continue;
                }

                result.game_objects.push_back(go);

                // Record every Component this GO owns (paired with the owning GO so we
                // can rebuild m_Components on the clone side later).
                for (ImmediatePtr<Component>& comp_ptr : go->getComponents())
                {
                    Component* comp = comp_ptr;
                    if (comp != nullptr)
                    {
                        result.components.push_back(comp);
                        result.component_owners.push_back(go);
                    }
                }

                // Descend into the Transform child chain — this is the canonical scene-
                // graph topology for prefab subtrees.
                TransformComponent* tc = go->tryGetComponent(TransformComponent);
                if (tc != nullptr)
                {
                    for (size_t i = 0, count = tc->GetChildCount(); i < count; ++i)
                    {
                        TransformComponent* child_tc = tc->GetChild(i);
                        if (child_tc == nullptr)
                        {
                            continue;
                        }
                        // The child's owning GameObject is reachable through
                        // Component::m_ParentObject (already populated for source-side
                        // objects via their original load path). If for some reason it
                        // isn't, we'd need to scan all known GOs — but in practice the
                        // source PrefabAsset has gone through postLoadResource long
                        // before InstantiateAsPrefab is called.
                        GameObject* child_go = nullptr;
                        // Component has m_ParentObject as a protected raw pointer; we
                        // can't access it directly from here, but the convention is
                        // that any TransformComponent in a well-formed scene tree has
                        // an owning GO. We rely on the caller having driven
                        // postLoadResource for the source asset; since we can't
                        // observe m_ParentObject protected member, we fall back to
                        // searching by ImmediatePtr equality across discovered GOs.
                        // To avoid an O(N²) blow-up, we instead keep a side map.
                        (void)child_go;
                        // Defer the resolution: stash the child Transform on the
                        // worklist as a "pending" entry. We'll resolve owning-GO via
                        // a second-pass map below.
                        // -- simpler: after the first DFS over GOs, enumerate every
                        //    GO's own TransformComponent and register {tc → go} into a
                        //    table; then for each child_tc look up the owning GO.
                        (void)child_tc;
                    }
                }
            }

            // Second pass: for every TransformComponent we recorded among `components`,
            // map it back to its owning GameObject. Then walk the child chains again to
            // realise the BFS expansion. (This is the simplest way to avoid touching the
            // protected Component::m_ParentObject member.)
            std::unordered_map<TransformComponent*, GameObject*> tc_to_owner;
            for (size_t i = 0; i < result.components.size(); ++i)
            {
                if (auto* tc = dynamic_cast<TransformComponent*>(result.components[i]))
                {
                    tc_to_owner[tc] = result.component_owners[i];
                }
            }

            // Now do the actual transitive-children expansion using tc_to_owner.
            std::vector<GameObject*> expanded_gos = result.game_objects;  // copy initial seed
            std::unordered_set<GameObject*> expanded_visited(expanded_gos.begin(), expanded_gos.end());

            // Simple iterative expansion over GOs not yet visited via the map.
            for (size_t cursor = 0; cursor < expanded_gos.size(); ++cursor)
            {
                GameObject* go = expanded_gos[cursor];
                if (go == nullptr)
                {
                    continue;
                }
                TransformComponent* tc = go->tryGetComponent(TransformComponent);
                if (tc == nullptr)
                {
                    continue;
                }
                for (size_t i = 0, count = tc->GetChildCount(); i < count; ++i)
                {
                    TransformComponent* child_tc = tc->GetChild(i);
                    if (child_tc == nullptr)
                    {
                        continue;
                    }
                    auto it = tc_to_owner.find(child_tc);
                    if (it == tc_to_owner.end())
                    {
                        continue;
                    }
                    GameObject* child_go = it->second;
                    if (child_go == nullptr || !expanded_visited.insert(child_go).second)
                    {
                        continue;
                    }
                    expanded_gos.push_back(child_go);
                    // Also append the child's components so they enter the clone set.
                    for (ImmediatePtr<Component>& comp_ptr : child_go->getComponents())
                    {
                        Component* comp = comp_ptr;
                        if (comp != nullptr)
                        {
                            result.components.push_back(comp);
                            result.component_owners.push_back(child_go);
                            if (auto* ctc = dynamic_cast<TransformComponent*>(comp))
                            {
                                tc_to_owner[ctc] = child_go;
                            }
                        }
                    }
                }
            }

            result.game_objects = std::move(expanded_gos);
            return result;
        }

        /// Re-wire the clone-side GameObject's m_Components ImmediatePtr list using the
        /// source→clone pointer map. The clone GameObject itself was produced via
        /// CopyPlainFields, which leaves m_Components empty (Read can't restore the raw
        /// ImmediatePtr m_Ptr) — so we rebuild it from scratch.
        void RebuildCloneComponents(
            GameObject* source_go,
            GameObject* clone_go,
            const std::unordered_map<Object*, Object*>& ptr_map)
        {
            for (ImmediatePtr<Component>& src_comp_ptr : source_go->getComponents())
            {
                Component* src_comp = src_comp_ptr;
                if (src_comp == nullptr)
                {
                    continue;
                }
                auto it = ptr_map.find(static_cast<Object*>(src_comp));
                if (it == ptr_map.end())
                {
                    LOG_WARNING(ZPrefab, "Clone: source component has no clone counterpart — skipped");
                    continue;
                }
                clone_go->addComponent(static_cast<Component*>(it->second));
            }
        }

        /// Re-wire TransformComponent.m_Parent and m_Children PPtrs to point at the
        /// clone-side TransformComponents. Does so by inspecting the SOURCE transform's
        /// pointer structure and translating each PPtr through ptr_map.
        void RebuildCloneTransformLinks(
            TransformComponent* source_tc,
            TransformComponent* clone_tc,
            const std::unordered_map<Object*, Object*>& ptr_map)
        {
            // m_Parent: SetParent rebuilds both the parent's children list and the child's
            // local-pose <-> world-pose accounting. Since the clone hierarchy is being
            // built from scratch, we want worldPositionStays=false (keep local pose
            // verbatim — the round-trip already copied the local Transform fields).
            if (TransformComponent* src_parent = source_tc->GetParent())
            {
                auto it = ptr_map.find(static_cast<Object*>(src_parent));
                if (it != ptr_map.end())
                {
                    clone_tc->SetParent(static_cast<TransformComponent*>(it->second), /*worldPositionStays=*/false);
                }
                // If the parent isn't in ptr_map, the source parent lives outside the
                // cloned subtree — leave the clone parent unset (subtree root case).
            }
            // m_Children rebuild is a side-effect of every child's SetParent call;
            // calling SetParent on children traverses up to mutate their parent's
            // m_Children list. Done implicitly when we iterate other clones.
        }

    }  // namespace

    CloneResult CloneSubtree(GameObject* source_root)
    {
        CloneResult result;
        if (source_root == nullptr)
        {
            LOG_ERROR(ZPrefab, "EditorPrefabCloner::CloneSubtree: null source_root");
            return result;
        }

        // 1) Walk the source subtree to enumerate every Object we'll need to clone.
        SourceWalkResult walk = WalkSourceTree(source_root);

        // 2) For every source GameObject and Component, allocate an empty clone and
        //    immediately copy its plain (non-pointer) fields. We reserve ptr_map and
        //    the parallel arrays in advance so growing them doesn't invalidate the
        //    pointer references we hand back.
        const size_t total_objects = walk.game_objects.size() + walk.components.size();
        result.sources.reserve(total_objects);
        result.clones.reserve(total_objects);
        result.ptr_map.reserve(total_objects);

        auto allocate_and_copy = [&](Object* source) -> Object* {
            Object* clone = TransferUtility::CloneObjectViaSerialization(*source);
            if (clone == nullptr)
            {
                LOG_ERROR(ZPrefab, "EditorPrefabCloner: CloneObjectViaSerialization failed for type '{}'", source->GetTypeName());
                return nullptr;
            }
            result.sources.push_back(source);
            result.clones.push_back(clone);
            result.ptr_map[source] = clone;
            return clone;
        };

        for (GameObject* src_go : walk.game_objects)
        {
            if (src_go == nullptr)
                continue;
            if (allocate_and_copy(src_go) == nullptr)
            {
                return CloneResult {};  // bail early — partial clone is worse than no clone
            }
        }
        for (Component* src_comp : walk.components)
        {
            if (src_comp == nullptr)
                continue;
            if (allocate_and_copy(src_comp) == nullptr)
            {
                return CloneResult {};
            }
        }

        // 3) Rewire pointer structure on the clone side using ptr_map.
        //    3a) GameObject.m_Components — rebuild from scratch (Round-trip cleared it).
        for (size_t i = 0; i < walk.game_objects.size(); ++i)
        {
            GameObject* src_go = walk.game_objects[i];
            if (src_go == nullptr)
                continue;
            auto it = result.ptr_map.find(src_go);
            if (it == result.ptr_map.end())
                continue;
            GameObject* clone_go = static_cast<GameObject*>(it->second);
            RebuildCloneComponents(src_go, clone_go, result.ptr_map);
        }

        //    3b) TransformComponent.m_Parent + m_Children — relink via SetParent calls,
        //        which is the official entry point that maintains the parent's
        //        children list invariant.
        for (size_t i = 0; i < walk.components.size(); ++i)
        {
            Component* src_comp = walk.components[i];
            auto* src_tc = dynamic_cast<TransformComponent*>(src_comp);
            if (src_tc == nullptr)
                continue;
            auto it = result.ptr_map.find(static_cast<Object*>(src_tc));
            if (it == result.ptr_map.end())
                continue;
            auto* clone_tc = static_cast<TransformComponent*>(it->second);
            RebuildCloneTransformLinks(src_tc, clone_tc, result.ptr_map);
        }

        // 4) Resolve the clone root.
        auto root_it = result.ptr_map.find(source_root);
        if (root_it == result.ptr_map.end())
        {
            LOG_ERROR(ZPrefab, "EditorPrefabCloner: clone_root resolution failed (missing in ptr_map)");
            return CloneResult {};
        }
        result.clone_root = static_cast<GameObject*>(root_it->second);

        LOG_INFO(ZPrefab, "EditorPrefabCloner: cloned subtree — {} GameObjects, {} Components", static_cast<int>(walk.game_objects.size()), static_cast<int>(walk.components.size()));
        return result;
    }

}  // namespace prefab_editor
