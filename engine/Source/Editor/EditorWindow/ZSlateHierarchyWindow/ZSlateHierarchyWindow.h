#pragma once

#include "Editor/EditorWindow/EditorWindow.h"
#include "Editor/EditorSceneManager/EditorSceneManager.h"  // GObjectID, k_invalid_gobject_id
#include "Editor/Menu/ZSlatePopupMenu.h"                   // reusable context-menu overlay
#include "Runtime/Slate/Core/SlatePaint.h"
#include "Runtime/UI/Render/UIRenderer.h"
#include "Runtime/Slate/Application/SlateInput.h"
#include "Runtime/Slate/Core/SlateGeometry.h"  // Vector2

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Level;

namespace ZSlate
{
class SWidget;
class SScrollBox;
class SMenu;
}

// A ZSlate-rendered, dockable Scene Hierarchy (default panel at dock title
// "Hierarchy"). Browse the GameObject tree, expand/collapse, click to select.
// Selection is shared via EditorSelection so the Inspector updates in lockstep.
//
// Right-click a row (or the blank area) for Create/Delete; drag a row onto another
// to reparent. Inline rename is not implemented yet (was on the legacy ImGui panel).
class ZSlateHierarchyWindow : public EditorWindow
{
public:
    explicit ZSlateHierarchyWindow(EditorUI* editor_ui);
    void OnGUI() override;
    bool SupportsNativeHosting() const override { return true; }

private:
    struct TreeData
    {
        std::unordered_map<GObjectID, std::vector<GObjectID>> children_by_parent;
        std::vector<GObjectID> roots;
    };

    TreeData BuildTree(Level* level) const;
    uint64_t ComputeSignature(Level* level, const TreeData& tree, GObjectID selected) const;
    void Rebuild(Level* level, const TreeData& tree, GObjectID selected, float scale);
    void AddNodeRows(Level* level,
                     const TreeData& tree,
                     GObjectID object_id,
                     GObjectID selected,
                     int depth,
                     float scale,
                     const std::shared_ptr<ZSlate::SScrollBox>& list);
    void BuildMessage(const char* text, float scale);

    bool IsCollapsed(GObjectID id) const { return m_Collapsed.count(id) != 0; }

    // Context menu + scene-edit actions (proves the ZSlate menu/drag primitives).
    void OpenContextMenu(GObjectID context_object, const Vector2& screen_pos, float scale);
    GObjectID CreateEmpty(Level* level, GObjectID parent);
    void DeleteObject(GObjectID object_id);
    void NotifyHierarchyStructureChanged(GObjectID expand_parent_id = k_invalid_gobject_id);

    std::shared_ptr<ZSlate::SWidget> m_Root;
    std::unordered_set<GObjectID> m_Collapsed;

    ZSlate::SlateInputRouter m_Input;

    // Per-window context menu overlay (reusable anchored popup w/ submenu stack).
    ZSlate::ZSlatePopupMenu m_Popup;

    // Right-click on a row records the target here; OnGUI opens the menu on the
    // right-button-up edge (so the row-vs-blank distinction is unambiguous).
    GObjectID m_PendingContextObject {k_invalid_gobject_id};
    Vector2 m_PendingContextPos {0.0f, 0.0f};
    bool m_HasPendingContext {false};

    bool m_PrevRightDown {false};
    bool m_PrevLeftDown {false};

    float m_BuiltScale {-1.0f};
    uint64_t m_BuiltSignature {0};
    bool m_ForceRebuild {true};
};
