#pragma once

#include "Runtime/Core/Math/Vector2.h"
#include "Runtime/UI/Core/UITypes.h"  // UIRect

#include <memory>
#include <string>
#include <vector>

// =====================================================================================
// DockNode.h -- native ZSlate docking tree node.
// -------------------------------------------------------------------------------------
// First building block of the native ZSlate dock space that replaces ImGui's
// DockSpace (see doc/ui_system/ZSLATE_NATIVE_DOCK_PLAN.md). A DockNode is a binary
// tree node that is EITHER:
//   - a split:  ChildA && ChildB are non-null, Tabs is empty. The node divides its
//               rectangle in two along Orientation at SplitRatio.
//   - a leaf:   ChildA == ChildB == nullptr, Tabs holds the docked panels (one tab
//               strip; ActiveTab selects which panel's content is visible).
// An empty dock space is a single leaf with no tabs.
//
// This header is pure data + layout state -- no rendering, no input, no ImGui. The
// host (a later phase) walks the tree, paints splitters/tab-strips/frames through the
// ZSlate UIRenderer, and positions each visible panel into the leaf's ContentRect.
// Keeping it free of GPU/ImGui dependencies makes the layout math unit-testable.
// =====================================================================================

namespace EditorDock
{
    // Split axis. Horizontal places ChildA to the LEFT of ChildB (the splitter is a
    // vertical bar, dragging it moves along X). Vertical stacks ChildA ABOVE ChildB
    // (horizontal splitter bar, dragging moves along Y).
    enum class EOrientation
    {
        Horizontal,
        Vertical
    };

    // Which edge of a leaf a newly-docked panel attaches to when splitting.
    enum class EDockDir
    {
        Left,
        Right,
        Top,
        Bottom,
        Center  // add as a tab to the existing leaf (no split)
    };

    // One panel reference inside a leaf's tab strip. PanelId is the stable identifier
    // matching EditorWindow::m_Title / EditorLayoutWindowIds::kXxx.
    struct DockTab
    {
        std::string PanelId;
    };

    struct DockNode
    {
        // --- Split fields (meaningful only when !IsLeaf()) -----------------------
        EOrientation Orientation {EOrientation::Horizontal};
        float SplitRatio {0.5f};  // fraction of the primary axis given to ChildA, in (0,1)
        std::unique_ptr<DockNode> ChildA;
        std::unique_ptr<DockNode> ChildB;

        // --- Leaf fields (meaningful only when IsLeaf()) -------------------------
        std::vector<DockTab> Tabs;
        int ActiveTab {0};

        // --- Per-frame computed geometry (filled by DockTree::ComputeGeometry) ----
        UIRect NodeRect {};      // full rectangle owned by this node
        UIRect ContentRect {};   // leaf: panel area under the tab strip; split: == NodeRect
        UIRect TabStripRect {};  // leaf: the tab-strip band along the top; split: empty
        UIRect SplitterRect {};  // split: the draggable gap between ChildA and ChildB; leaf: empty

        // Per-tab screen rects, filled by DockHost while painting the tab strip and used
        // for tab hit-testing (input). Index-aligned with Tabs; empty until first paint.
        std::vector<UIRect> TabRects;

        // --- P7 transient tab-strip widgets (filled by DockHost::LayoutTabs; NOT serialized) ---
        // Horizontal scroll of the tab strip, in pixels, when the tabs overflow the strip
        // width. Clamped to [0, maxScroll] by the layout pass each frame.
        float TabScrollOffset {0.0f};
        // Scroll arrow buttons at the right of the strip; non-empty only while overflowing.
        UIRect ScrollLeftRect {};
        UIRect ScrollRightRect {};
        // Close ("x") button on the active tab; non-empty only when a tab is active.
        UIRect ActiveTabCloseRect {};

        bool IsLeaf() const { return ChildA == nullptr && ChildB == nullptr; }

        // True when this leaf holds no panels (the only legal empty state is the root).
        bool IsEmptyLeaf() const { return IsLeaf() && Tabs.empty(); }
    };
}  // namespace EditorDock
