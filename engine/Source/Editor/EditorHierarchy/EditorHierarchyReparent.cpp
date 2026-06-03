#include "Editor/EditorHierarchy/EditorHierarchyReparent.h"

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Function/Framework/Level/Level.h"
#include "Runtime/Function/Framework/World/WorldManager.h"

namespace EditorHierarchyReparent
{
    GObjectID GetParentId(const std::shared_ptr<GameObject>& object)
    {
        if (object == nullptr)
        {
            return k_invalid_gobject_id;
        }

        Transform* transform = object->tryGetComponent(Transform);
        if (transform == nullptr)
        {
            return k_invalid_gobject_id;
        }

        Transform* parent_transform = transform->GetParent();
        if (parent_transform == nullptr)
        {
            return k_invalid_gobject_id;
        }

        GameObject* parent_object = parent_transform->GetParentObject();
        return parent_object != nullptr ? parent_object->GetID() : k_invalid_gobject_id;
    }

    bool WouldCreateCycle(Level* level, GObjectID child_id, GObjectID new_parent_id)
    {
        if (level == nullptr || child_id == k_invalid_gobject_id)
        {
            return true;
        }

        if (new_parent_id == k_invalid_gobject_id)
        {
            return false;
        }

        if (child_id == new_parent_id)
        {
            return true;
        }

        GObjectID probe_id = new_parent_id;
        while (probe_id != k_invalid_gobject_id)
        {
            if (probe_id == child_id)
            {
                return true;
            }

            probe_id = GetParentId(level->GetGObjectByID(probe_id).lock());
        }

        return false;
    }

    bool Reparent(Level* level, GObjectID child_id, GObjectID new_parent_id)
    {
        if (level == nullptr || child_id == k_invalid_gobject_id)
        {
            return false;
        }

        if (WouldCreateCycle(level, child_id, new_parent_id))
        {
            LOG_WARNING(ZEditor,
                        "Hierarchy reparent: refused cycle (child={}, parent={})",
                        child_id,
                        new_parent_id);
            return false;
        }

        const std::shared_ptr<GameObject> child_object = level->GetGObjectByID(child_id).lock();
        if (child_object == nullptr)
        {
            return false;
        }

        if (new_parent_id == k_invalid_gobject_id)
        {
            if (Transform* child_transform = child_object->tryGetComponent(Transform))
            {
                child_transform->SetParent(nullptr, true);
                GET_SYSTEM(WorldManager)->MarkCurrentLevelDirty();
                return true;
            }

            return false;
        }

        const std::shared_ptr<GameObject> parent_object = level->GetGObjectByID(new_parent_id).lock();
        if (parent_object == nullptr)
        {
            return false;
        }

        Transform* child_transform = child_object->tryGetComponent(Transform);
        Transform* parent_transform = parent_object->tryGetComponent(Transform);
        if (child_transform == nullptr || parent_transform == nullptr)
        {
            LOG_WARNING(ZEditor,
                        "Hierarchy reparent: '{}' has no Transform parent hook",
                        child_object->GetName().c_str());
            return false;
        }

        child_transform->SetParent(parent_transform, true);
        GET_SYSTEM(WorldManager)->MarkCurrentLevelDirty();
        return true;
    }

    size_t ReparentMany(Level* level,
                        const std::vector<GObjectID>& child_ids,
                        GObjectID new_parent_id)
    {
        if (level == nullptr || child_ids.empty())
        {
            return 0;
        }

        size_t success_count = 0;
        for (const GObjectID child_id : child_ids)
        {
            if (child_id == k_invalid_gobject_id || child_id == new_parent_id)
            {
                continue;
            }

            if (Reparent(level, child_id, new_parent_id))
            {
                ++success_count;
            }
        }

        return success_count;
    }

    GObjectID SanitizeHierarchyDropParent(GObjectID proposed_parent_id,
                                          GObjectID dragged_id,
                                          const std::vector<GObjectID>& selection)
    {
        (void)selection;
        if (proposed_parent_id == k_invalid_gobject_id || proposed_parent_id == dragged_id)
        {
            return k_invalid_gobject_id;
        }

        return proposed_parent_id;
    }

    size_t ReparentDraggedHierarchyDrop(Level* level,
                                        GObjectID dragged_id,
                                        GObjectID drop_parent_id,
                                        const std::vector<GObjectID>& selection)
    {
        if (level == nullptr || dragged_id == k_invalid_gobject_id)
        {
            return 0;
        }

        drop_parent_id = SanitizeHierarchyDropParent(drop_parent_id, dragged_id, selection);

        std::vector<GObjectID> ids_to_move;
        ids_to_move.reserve(selection.size() + 1);

        bool dragged_in_selection = false;
        for (const GObjectID selected_id : selection)
        {
            if (selected_id == k_invalid_gobject_id)
            {
                continue;
            }

            if (selected_id == dragged_id)
            {
                dragged_in_selection = true;
            }

            ids_to_move.push_back(selected_id);
        }

        if (!dragged_in_selection || ids_to_move.size() <= 1)
        {
            ids_to_move.clear();
            ids_to_move.push_back(dragged_id);
        }

        return ReparentMany(level, ids_to_move, drop_parent_id);
    }
}  // namespace EditorHierarchyReparent
