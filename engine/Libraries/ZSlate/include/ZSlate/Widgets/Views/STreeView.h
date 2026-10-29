#pragma once

#include "ZSlate/Widgets/SPanel.h"
#include "ZSlate/Widgets/Layout/SScrollBox.h"
#include "ZSlate/Widgets/Text/STextBlock.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ZSlate
{

// =============================================================================
// STreeView<T> — hierarchical tree view (UE STreeView analogue)
// =============================================================================
//
// Displays an expandable tree. Each node has children and an expanded state.
//
// Usage:
//   struct MyNode { std::string label; std::vector<MyNode> children; };
//   auto tree = std::make_shared<STreeView<MyNode>>();
//   tree->RootNodes.push_back(root);
//   tree->OnGetLabel = [](const MyNode& n) { return n.label; };
//   tree->OnGetChildren = [](const MyNode& n) -> const auto& { return n.children; };
//
template <typename T>
class STreeView : public SPanel
{
public:
    // ---- Data source --------------------------------------------------------

    std::vector<T> RootNodes;

    // Callbacks
    std::function<std::string(const T&)> OnGetLabel;
    std::function<const std::vector<T>*(const T&)> OnGetChildren;
    std::function<void(const T&)> OnSelectionChanged;

    // Selected node index (flat index into the visible list)
    mutable int32_t SelectedIndex {-1};

    // ---- Style ---------------------------------------------------------------

    float RowHeight   {20.0f};
    float IndentWidth {16.0f};
    float FontSize    {13.0f};

    // ---- Widget overrides ----------------------------------------------------

    Vector2 ComputeDesiredSize() const override
    {
        RebuildFlatList();
        return Vector2(300.0f, m_FlatList.size() * RowHeight);
    }

    void OnArrangeChildren(const FGeometry& AllottedGeometry,
                           std::vector<FArrangedChild>& OutArrangedChildren) const override
    {
        RebuildFlatList();
        const UIRect view = AllottedGeometry.ToRect();

        for (size_t i = 0; i < m_FlatList.size(); ++i)
        {
            float y = view.y + static_cast<float>(i) * RowHeight;
            FGeometry rowGeom = AllottedGeometry.MakeChild(
                Vector2(0.0f, static_cast<float>(i) * RowHeight),
                Vector2(view.w, RowHeight));
            // Store index for paint callback
            auto row = std::make_shared<STreeRow>(*this, static_cast<int32_t>(i));
            OutArrangedChildren.emplace_back(row, rowGeom);
        }
    }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (!ctx.Renderer) return;
        RebuildFlatList();

        const UIRect view = geom.ToRect();

        for (size_t i = 0; i < m_FlatList.size(); ++i)
        {
            float y = view.y + static_cast<float>(i) * RowHeight;
            UIRect rowRect(view.x, y, view.w, RowHeight);

            const auto& entry = m_FlatList[i];

            // Selection highlight
            if (static_cast<int32_t>(i) == SelectedIndex)
                ctx.Renderer->DrawQuad(rowRect, UIColor(0.18f, 0.35f, 0.62f, 1.0f));
            else if (static_cast<int32_t>(i) == m_HoveredIndex)
                ctx.Renderer->DrawQuad(rowRect, UIColor(0.14f, 0.14f, 0.18f, 1.0f));

            // Indent
            float indent = static_cast<float>(entry.Depth) * IndentWidth + 4.0f;

            // Expand/collapse arrow (if has children)
            bool hasChildren = OnGetChildren && OnGetChildren(*entry.Node) && !OnGetChildren(*entry.Node)->empty();
            if (hasChildren)
            {
                const char* arrow = entry.Expanded ? "\xE2\x96\xBC" : "\xE2\x96\xB6";  // ▼ or ▶
                ctx.Renderer->DrawTextLabel(
                    UIRect(view.x + indent, y, IndentWidth, RowHeight),
                    arrow, FontSize, UIColor(0.7f, 0.7f, 0.7f, 1.0f),
                    TextAnchor::MiddleCenter, TextWrapMode::NoWrap);
            }

            // Label
            float labelX = view.x + indent + IndentWidth;
            std::string label = OnGetLabel ? OnGetLabel(*entry.Node) : "(no label)";
            ctx.Renderer->DrawTextLabel(
                UIRect(labelX, y, view.w - labelX - 4.0f, RowHeight),
                label, FontSize,
                static_cast<int32_t>(i) == SelectedIndex
                    ? UIColor(1.0f, 1.0f, 1.0f, 1.0f)
                    : UIColor(0.85f, 0.88f, 0.94f, 1.0f),
                TextAnchor::MiddleLeft, TextWrapMode::NoWrap);

            // Divider line
            ctx.Renderer->DrawRect(UIRect(view.x, y + RowHeight - 1.0f, view.w, 1.0f),
                                   UIColor(0.08f, 0.08f, 0.10f, 1.0f), 1.0f);
        }
    }

    // ---- Input ---------------------------------------------------------------

    void OnMouseMove(const Vector2& pos) override
    {
        const UIRect r = GetCachedGeometry().ToRect();
        int32_t idx = HitTestRow(pos, r);
        if (idx != m_HoveredIndex)
        {
            m_HoveredIndex = idx;
        }
    }

    void OnMouseLeave() override { m_HoveredIndex = -1; }

    FReply OnMouseButtonDown(const Vector2& pos, int btn) override
    {
        if (btn != 0 || m_FlatList.empty()) return FReply::Unhandled();

        const UIRect r = GetCachedGeometry().ToRect();
        int32_t idx = HitTestRow(pos, r);
        if (idx < 0 || idx >= static_cast<int32_t>(m_FlatList.size()))
            return FReply::Unhandled();

        auto& entry = m_FlatList[idx];

        // Check if clicking expand/collapse arrow
        float arrowX = r.x + static_cast<float>(entry.Depth) * IndentWidth + 4.0f
                       + IndentWidth * 0.5f;
        bool hasChildren = OnGetChildren && OnGetChildren(*entry.Node) && !OnGetChildren(*entry.Node)->empty();
        if (hasChildren && pos.x > r.x + static_cast<float>(entry.Depth) * IndentWidth + 4.0f
            && pos.x < arrowX + IndentWidth)
        {
            entry.Expanded = !entry.Expanded;
            m_FlatListDirty = true;
            return FReply::Handled();
        }

        // Select
        SelectedIndex = idx;
        if (OnSelectionChanged) OnSelectionChanged(*entry.Node);
        return FReply::Handled();
    }

    FReply OnMouseWheel(const Vector2&, float delta) override
    {
        if (m_FlatList.size() <= 1) return FReply::Unhandled();
        int32_t newIdx = std::clamp(SelectedIndex - static_cast<int32_t>(delta > 0 ? 1 : -1),
                                     0, static_cast<int32_t>(m_FlatList.size()) - 1);
        if (newIdx != SelectedIndex)
        {
            SelectedIndex = newIdx;
            if (OnSelectionChanged) OnSelectionChanged(*m_FlatList[SelectedIndex].Node);
        }
        return FReply::Handled();
    }

private:
    // ---- Flat list entry ----------------------------------------------------

    struct FlatEntry
    {
        const T* Node;
        int32_t Depth;
        mutable bool Expanded {false};
    };

    mutable std::vector<FlatEntry> m_FlatList;
    mutable bool m_FlatListDirty {true};
    mutable int32_t m_HoveredIndex {-1};

    void RebuildFlatList() const
    {
        if (!m_FlatListDirty) return;
        m_FlatList.clear();
        for (auto& root : RootNodes)
            FlattenNode(root, 0);
        m_FlatListDirty = false;
    }

    void FlattenNode(const T& node, int32_t depth) const
    {
        m_FlatList.push_back({&node, depth, false});
        // Re-expand previously expanded nodes by checking if pointer matches
        // For simplicity, always collapsed after rebuild unless the node was
        // already in the old list as expanded.
        for (auto& old : m_OldFlatList)
        {
            if (old.Node == m_FlatList.back().Node && old.Expanded)
            {
                m_FlatList.back().Expanded = true;
                break;
            }
        }
        if (m_FlatList.back().Expanded)
        {
            auto* children = OnGetChildren ? OnGetChildren(node) : nullptr;
            if (children)
            {
                for (auto& child : *children)
                    FlattenNode(child, depth + 1);
            }
        }
    }

    mutable std::vector<FlatEntry> m_OldFlatList;  // snapshot for expand-state preservation

    int32_t HitTestRow(const Vector2& pos, const UIRect& view) const
    {
        if (pos.y < view.y) return -1;
        int32_t idx = static_cast<int32_t>((pos.y - view.y) / RowHeight);
        if (idx < 0 || idx >= static_cast<int32_t>(m_FlatList.size())) return -1;
        return idx;
    }

    // ---- STreeRow helper widget (needed for ArrangeChildren contract) --------

    struct STreeRow : public SWidget
    {
        const STreeView& Owner;
        int32_t Index;
        STreeRow(const STreeView& o, int32_t i) : Owner(o), Index(i) {}
        Vector2 ComputeDesiredSize() const override { return Vector2(300, Owner.RowHeight); }
    };
};

}  // namespace ZSlate
