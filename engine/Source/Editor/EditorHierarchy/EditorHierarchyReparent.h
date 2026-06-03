#pragma once

#include "Runtime/Function/Framework/Object/ObjectIdAllocator.h"

#include <memory>
#include <vector>

class GameObject;
class Level;

namespace EditorHierarchyReparent
{
    // Parent link used by the Hierarchy tree (UI RectTransform tree, else Transform).
    GObjectID GetParentId(const std::shared_ptr<GameObject>& object);

    // True if assigning `new_parent_id` as parent of `child_id` would create a cycle.
    bool WouldCreateCycle(Level* level, GObjectID child_id, GObjectID new_parent_id);

    // Reparent `child_id` under `new_parent_id`. `k_invalid_gobject_id` parent detaches to root.
    bool Reparent(Level* level, GObjectID child_id, GObjectID new_parent_id);

    // Reparent many objects (skips invalid ids, self-parent, and cycle cases).
    size_t ReparentMany(Level* level,
                        const std::vector<GObjectID>& child_ids,
                        GObjectID new_parent_id);

    // If `dragged_id` is part of `selection`, all selected objects move; else only `dragged_id`.
    size_t ReparentDraggedHierarchyDrop(Level* level,
                                        GObjectID dragged_id,
                                        GObjectID drop_parent_id,
                                        const std::vector<GObjectID>& selection);

    // Rejects parenting under self or under another object in the moving set.
    GObjectID SanitizeHierarchyDropParent(GObjectID proposed_parent_id,
                                          GObjectID dragged_id,
                                          const std::vector<GObjectID>& selection);
}  // namespace EditorHierarchyReparent
