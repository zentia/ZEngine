#include "Runtime/Resource/Prefab/PrefabSubtreeCloner.h"

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/BaseClasses/Object.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Serialize/TransferUtility.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"

#include <unordered_set>
#include <vector>
#include "Runtime/BaseClasses/Cast.h"

namespace prefab_clone
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

            // Breadth-first over Transform.m_Children. A child Transform names its
            // GameObject through Component::m_ParentObject: bound by PostLoadResource
            // for live objects and by PrefabUtility::LoadPrefabAsset for a prefab graph
            // read from disk. (The old walker never followed it: it only knew the owners
            // of GameObjects it had already visited, so it could not step from a parent
            // to its first child and every prefab cloned as its root alone.)
            std::unordered_set<GameObject*> visited;
            std::vector<GameObject*> queue {source_root};
            for (size_t cursor = 0; cursor < queue.size(); ++cursor)
            {
                GameObject* go = queue[cursor];
                if (go == nullptr || !visited.insert(go).second)
                    continue;

                result.game_objects.push_back(go);
                for (ImmediatePtr<Component>& comp_ptr : go->getComponents())
                {
                    Component* comp = comp_ptr;
                    if (comp != nullptr)
                    {
                        result.components.push_back(comp);
                        result.component_owners.push_back(go);
                    }
                }

                Transform* tc = go->tryGetComponent(Transform);
                if (tc == nullptr)
                    continue;
                for (size_t i = 0, count = tc->GetChildCount(); i < count; ++i)
                {
                    Transform* child_tc = tc->GetChild(i);
                    if (child_tc == nullptr)
                        continue;
                    GameObject* child_go = child_tc->GetParentObject();
                    if (child_go == nullptr)
                    {
                        LOG_WARNING(ZPrefab,
                                    "PrefabSubtreeCloner: a child Transform of '{}' has no resolvable GameObject; "
                                    "its subtree is not cloned",
                                    go->GetName().c_str());
                        continue;
                    }
                    queue.push_back(child_go);
                }
            }
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

        /// Re-wire Transform.m_Parent and m_Children PPtrs to point at the
        /// clone-side Transforms. Does so by inspecting the SOURCE transform's
        /// pointer structure and translating each PPtr through ptr_map.
        void RebuildCloneTransformLinks(
            Transform* source_tc,
            Transform* clone_tc,
            const std::unordered_map<Object*, Object*>& ptr_map)
        {
            // m_Parent: SetParent rebuilds both the parent's children list and the child's
            // local-pose <-> world-pose accounting. Since the clone hierarchy is being
            // built from scratch, we want worldPositionStays=false (keep local pose
            // verbatim — the round-trip already copied the local Transform fields).
            if (Transform* src_parent = source_tc->GetParent())
            {
                auto it = ptr_map.find(static_cast<Object*>(src_parent));
                if (it != ptr_map.end())
                {
                    clone_tc->SetParent(static_cast<Transform*>(it->second), /*worldPositionStays=*/false);
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
            LOG_ERROR(ZPrefab, "PrefabSubtreeCloner::CloneSubtree: null source_root");
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
                LOG_ERROR(ZPrefab, "PrefabSubtreeCloner: CloneObjectViaSerialization failed for type '{}'", source->GetTypeName());
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

        //    3b-0) The serialized copy carries the SOURCE's m_Father / m_Children
        //          links. SetParent appends to the parent's list, so without this a
        //          cloned parent ends up listing the source child AND the clone child.
        for (Object* clone_obj : result.clones)
        {
            if (auto* clone_tc = Cast<Transform>(clone_obj))
            {
                clone_tc->ClearCopiedHierarchyLinks();
            }
        }

        //    3b) Transform.m_Parent + m_Children — relink via SetParent calls,
        //        which is the official entry point that maintains the parent's
        //        children list invariant.
        for (size_t i = 0; i < walk.components.size(); ++i)
        {
            Component* src_comp = walk.components[i];
            auto* src_tc = Cast<Transform>(src_comp);
            if (src_tc == nullptr)
                continue;
            auto it = result.ptr_map.find(static_cast<Object*>(src_tc));
            if (it == result.ptr_map.end())
                continue;
            auto* clone_tc = static_cast<Transform*>(it->second);
            RebuildCloneTransformLinks(src_tc, clone_tc, result.ptr_map);
        }

        // 4) Resolve the clone root.
        auto root_it = result.ptr_map.find(source_root);
        if (root_it == result.ptr_map.end())
        {
            LOG_ERROR(ZPrefab, "PrefabSubtreeCloner: clone_root resolution failed (missing in ptr_map)");
            return CloneResult {};
        }
        result.clone_root = static_cast<GameObject*>(root_it->second);

        LOG_INFO(ZPrefab, "PrefabSubtreeCloner: cloned subtree — {} GameObjects, {} Components", static_cast<int>(walk.game_objects.size()), static_cast<int>(walk.components.size()));
        return result;
    }

}  // namespace prefab_clone
