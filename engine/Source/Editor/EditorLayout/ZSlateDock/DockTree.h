#pragma once

#include "Editor/EditorLayout/ZSlateDock/DockNode.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

// =====================================================================================
// DockTree.h -- native ZSlate dock space layout/state (no rendering, no ImGui).
// -------------------------------------------------------------------------------------
// Owns the binary DockNode tree and exposes the operations the host needs:
//   - ComputeGeometry: solve NodeRect / ContentRect / TabStripRect / SplitterRect for
//     every node given the dock host's rectangle.
//   - AddTab / RemovePanel / SplitLeaf / MovePanel: structural edits (drag-to-dock).
//   - SetSplitRatioFromDrag: splitter resize.
//   - HitTestSplitter / FindPanelLeaf / LeafAt: hit-testing for input.
//   - SerializeToJson / DeserializeFromJson: persistence (replaces the docking section
//     of imgui.ini; mirrors the .zlayout.json convention).
//
// This is the model layer; it is GPU/ImGui-free so it can be exercised in isolation.
// =====================================================================================

namespace EditorDock
{
    class DockTree
    {
    public:
        // Layout metrics in logical pixels. The host multiplies by DPI scale before
        // calling SetMetrics (so the tree math stays scale-agnostic).
        struct Metrics
        {
            float SplitterThickness {4.0f};
            float TabStripHeight {24.0f};
            float MinNodeExtent {48.0f};  // smallest extent a split child may shrink to
        };

        DockTree();

        void SetMetrics(const Metrics& metrics) { m_Metrics = metrics; }
        const Metrics& GetMetrics() const { return m_Metrics; }

        DockNode* Root() { return m_Root.get(); }
        const DockNode* Root() const { return m_Root.get(); }

        // Recompute geometry for the whole tree inside `rect`.
        void ComputeGeometry(const UIRect& rect);

        // Tree walks (call AFTER ComputeGeometry to read rects).
        void ForEachLeaf(const std::function<void(DockNode&)>& fn);
        void ForEachNode(const std::function<void(DockNode&)>& fn);

        // Find the leaf currently holding `panel_id` (nullptr if not docked).
        DockNode* FindPanelLeaf(const std::string& panel_id);

        // Leaf whose ContentRect/TabStripRect contains `p` (nullptr if none). Requires
        // a prior ComputeGeometry.
        DockNode* LeafAt(const Vector2& p);

        // Add `panel_id` as a new tab in `leaf` and make it active. No-op (returns the
        // existing index) if already present.
        int AddTab(DockNode& leaf, const std::string& panel_id);

        // Remove `panel_id` from wherever it lives. Empty leaves collapse: the sibling
        // subtree is promoted into the parent's slot. Returns true if anything changed.
        bool RemovePanel(const std::string& panel_id);

        // Split `leaf` so `panel_id` lands on the `dir` edge (Center => AddTab instead).
        // `ratio` is the fraction of the split given to the NEW panel's side. Returns the
        // leaf that ends up holding `panel_id`.
        DockNode* SplitLeaf(DockNode& leaf, EDockDir dir, const std::string& panel_id, float ratio = 0.5f);

        // Convenience for drag-to-dock: detach `panel_id` from its current leaf and
        // re-dock it onto `target` in direction `dir`. Returns the destination leaf.
        DockNode* MovePanel(const std::string& panel_id, DockNode& target, EDockDir dir, float ratio = 0.5f);

        // Splitter drag: set a split node's ratio, clamped so neither child shrinks below
        // MinNodeExtent for the node's current NodeRect.
        void SetSplitRatioFromDrag(DockNode& split_node, float new_ratio);

        // Hit-test the splitter gaps. Returns the split node whose SplitterRect contains
        // `p` (nullptr if none). Requires a prior ComputeGeometry.
        DockNode* HitTestSplitter(const Vector2& p);

        // Every panel id present anywhere in the tree (leaf tab order, depth-first).
        std::vector<std::string> CollectPanelIds() const;

        // True if `panel_id` is docked somewhere.
        bool HasPanel(const std::string& panel_id) const;

        // Reset to a single empty leaf.
        void Clear();

        // Reset to a single leaf holding `panel_ids` (ActiveTab = 0).
        void ResetToSingleLeaf(const std::vector<std::string>& panel_ids);

        // Persistence. Format: {"version":1,"root":<node>} where <node> is either
        // {"type":"leaf","active":N,"tabs":[...]} or
        // {"type":"split","orientation":"h|v","ratio":R,"a":<node>,"b":<node>}.
        std::string SerializeToJson() const;
        bool DeserializeFromJson(const std::string& json);

        // Dirty tracking for debounced persistence. Structural edits / ratio drags / tab
        // activation set the flag; the host's autosave consumes it so it only serializes on
        // an actual change instead of every frame. DeserializeFromJson does NOT set it (the
        // tree then equals what is on disk). ActiveTab changes done directly on a DockNode
        // (outside these methods) must call MarkDirty() at the call site.
        void MarkDirty() { m_Dirty = true; }
        bool IsDirty() const { return m_Dirty; }
        bool ConsumeDirty()
        {
            const bool was_dirty = m_Dirty;
            m_Dirty = false;
            return was_dirty;
        }

    private:
        void ComputeNode(DockNode& node, const UIRect& rect);

        // Recursive remove that collapses empty leaves by promoting the sibling. Returns
        // true if `panel_id` was found under `owner`.
        bool RemoveFromOwner(std::unique_ptr<DockNode>& owner, const std::string& panel_id);

        std::unique_ptr<DockNode> m_Root;
        Metrics m_Metrics;
        bool m_Dirty {false};
    };
}  // namespace EditorDock
