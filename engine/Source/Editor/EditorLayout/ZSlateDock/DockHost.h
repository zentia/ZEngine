#pragma once

#include "Editor/EditorLayout/ZSlateDock/DockNode.h"
#include "Runtime/Core/Math/Vector2.h"
#include "Runtime/UI/Core/UITypes.h"  // UIRect, ZSlate::UIColor

#include <string>

class BatchedUIRenderer;

// =====================================================================================
// DockHost.h -- render-only painter for a native ZSlate dock tree (PHASE 2).
// -------------------------------------------------------------------------------------
// Walks a geometry-solved DockTree and paints, through the shared BatchedUIRenderer:
//   - per-leaf panel content background + 1px frame,
//   - per-leaf tab strip with one tab per docked panel (active / inactive / hover),
//   - per-split draggable splitter bar.
//
// This phase performs NO input handling and NO structural edits -- the `mouse`
// argument only drives hover highlighting so the chrome looks live. Splitter drag,
// tab activation, and drag-to-dock land in later phases (see
// doc/ui_system/ZSLATE_NATIVE_DOCK_PLAN.md P4/P5). Keeping render and input separate
// mirrors how the rest of the editor's ZSlate chrome is structured.
// =====================================================================================

namespace EditorDock
{
    class DockTree;

    class DockHost
    {
    public:
        struct Style
        {
            ZSlate::UIColor PanelBg {0.12f, 0.13f, 0.15f, 1.0f};
            ZSlate::UIColor BorderColor {0.07f, 0.07f, 0.09f, 1.0f};
            ZSlate::UIColor TabStripBg {0.16f, 0.17f, 0.19f, 1.0f};
            ZSlate::UIColor TabActiveBg {0.26f, 0.28f, 0.32f, 1.0f};
            ZSlate::UIColor TabInactiveBg {0.20f, 0.21f, 0.24f, 1.0f};
            ZSlate::UIColor TabHoverBg {0.30f, 0.32f, 0.36f, 1.0f};
            ZSlate::UIColor TabText {0.70f, 0.73f, 0.78f, 1.0f};
            ZSlate::UIColor TabActiveText {0.96f, 0.97f, 0.99f, 1.0f};
            ZSlate::UIColor SplitterColor {0.10f, 0.10f, 0.12f, 1.0f};
            ZSlate::UIColor SplitterHoverColor {0.26f, 0.52f, 0.84f, 1.0f};
            ZSlate::UIColor ActiveTabAccent {0.26f, 0.52f, 0.84f, 1.0f};  // top accent bar on the active tab
            ZSlate::UIColor DropPreviewFill {0.26f, 0.52f, 0.84f, 0.35f};   // drag-to-dock target highlight
            ZSlate::UIColor DropPreviewBorder {0.36f, 0.62f, 0.94f, 0.95f};

            float TabPaddingX {12.0f};
            float TabGap {2.0f};
            float FontSize {14.0f};
        };

        DockHost() = default;

        const Style& GetStyle() const { return m_Style; }
        void SetStyle(const Style& style) { m_Style = style; }

        // Paint the opaque panel-area background fill for every leaf. Must be recorded
        // in a z-group STRICTLY BELOW the panel content (kZDockBackground) -- the fill is
        // opaque and would otherwise blank the panel bodies, since dock chrome is recorded
        // after the panels each frame. Render() (below) draws the rest of the chrome (border,
        // tab strip, tabs) which sits above the content, so it stays at kZForeground.
        void RenderBackgrounds(DockTree& tree, BatchedUIRenderer& renderer);

        // Paint a tree that has already been geometry-solved (DockTree::ComputeGeometry).
        // `scale` scales paddings + font; `mouse` is hover-only (no state mutation).
        // Draws border + tab strip + tabs only; the panel-area fill is RenderBackgrounds'.
        void Render(DockTree& tree, BatchedUIRenderer& renderer, float scale, const Vector2& mouse);

        // Fill every leaf's DockNode::TabRects without drawing. Lets the input layer
        // hit-test tabs before Render runs (uses the exact same layout math Render does).
        void LayoutTabs(DockTree& tree, BatchedUIRenderer& renderer, float scale);

        // Paint the drag-to-dock drop preview: the region `panel_id` would occupy if
        // dropped onto `target` in direction `dir` (half of NodeRect for an edge, the
        // whole ContentRect for Center). Translucent fill + accent border.
        void DrawDropPreview(BatchedUIRenderer& renderer, const DockNode& target, EDockDir dir) const;

    private:
        void RenderLeaf(DockNode& leaf, BatchedUIRenderer& renderer, float scale, const Vector2& mouse);
        void RenderSplitter(DockNode& split, BatchedUIRenderer& renderer, const Vector2& mouse);

        Style m_Style;
    };
}  // namespace EditorDock
