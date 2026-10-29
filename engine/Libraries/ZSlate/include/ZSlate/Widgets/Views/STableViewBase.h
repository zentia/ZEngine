#pragma once

#include "ZSlate/Widgets/SPanel.h"
#include "ZSlate/Widgets/Views/SHeaderRow.h"
#include "ZSlate/Widgets/Layout/SScrollBar.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <set>
#include <vector>

namespace ZSlate
{

// =============================================================================
// STableViewBase — non-templated base for list/table/tree views
// =============================================================================
//
// Provides scroll management, selection, scroll bar integration, and
// item layout. Templated views (SListView, STreeView, STileView) inherit
// from this to share scrolling and selection logic.
//
class STableViewBase : public SPanel
{
public:
    // ---- Configuration -------------------------------------------------------

    EOrientation Orientation {EOrientation::Vertical};
    float ItemHeight {24.0f};
    float ItemWidth  {0.0f};  // 0 = fill available width

    // ---- Selection -----------------------------------------------------------

    enum class ESelectionMode { None, Single, Multi };

    ESelectionMode SelectionMode {ESelectionMode::Single};

    bool IsSelected(int32_t index) const
    {
        return m_SelectedIndices.count(index) > 0;
    }

    void SetSelected(int32_t index, bool selected)
    {
        if (selected)
        {
            if (SelectionMode == ESelectionMode::Single)
                m_SelectedIndices.clear();
            m_SelectedIndices.insert(index);
        }
        else
        {
            m_SelectedIndices.erase(index);
        }
    }

    void ClearSelection() { m_SelectedIndices.clear(); }

    int32_t GetSelectedCount() const { return static_cast<int32_t>(m_SelectedIndices.size()); }

    // ---- Scroll --------------------------------------------------------------

    float GetScrollOffset() const { return m_ScrollOffset; }
    void SetScrollOffset(float offset) { m_ScrollOffset = offset; }

    void ScrollToTop() { m_ScrollOffset = 0.0f; }
    void ScrollToBottom(int32_t totalItems, float viewLength)
    {
        float totalLen = totalItems * ItemHeight;
        m_ScrollOffset = std::max(0.0f, totalLen - viewLength);
    }

    void ScrollBy(float delta)
    {
        m_ScrollOffset -= delta;
    }

    void ScrollIntoView(int32_t index, float viewLength)
    {
        float top = index * ItemHeight;
        float bot = top + ItemHeight;
        float curTop = m_ScrollOffset;
        float curBot = m_ScrollOffset + viewLength;

        if (top < curTop)
            m_ScrollOffset = top;
        else if (bot > curBot)
            m_ScrollOffset = bot - viewLength;
    }

    // ---- Scroll Bar ----------------------------------------------------------

    void SetScrollBar(std::shared_ptr<SScrollBar> bar) { m_ScrollBar = std::move(bar); }
    std::shared_ptr<SScrollBar> GetScrollBar() const { return m_ScrollBar; }

    // ---- Header Row ----------------------------------------------------------

    void SetHeaderRow(std::shared_ptr<SHeaderRow> header) { m_HeaderRow = std::move(header); }
    std::shared_ptr<SHeaderRow> GetHeaderRow() const { return m_HeaderRow; }

    // ---- Rebuild notification ------------------------------------------------

    void RequestRefresh() { m_NeedsRefresh = true; }
    bool IsPendingRefresh() const { return m_NeedsRefresh; }
    void CompleteRefresh() { m_NeedsRefresh = false; }

    // ---- Layout helpers (called by derived paint/arrange) --------------------

    float GetHeaderHeight() const
    {
        // The header is painted by the view, not routed as a child, so its
        // cached desired size is never refreshed; ask it directly.
        return m_HeaderRow ? m_HeaderRow->ComputeDesiredSize().y : 0.0f;
    }

    void ClampScrollOffset(int32_t totalItems, float viewLength)
    {
        float totalLen = totalItems * ItemHeight;
        m_ScrollOffset = std::clamp(m_ScrollOffset, 0.0f, std::max(0.0f, totalLen - viewLength));
    }

    void SyncScrollBar(int32_t totalItems, float viewLength)
    {
        if (!m_ScrollBar) return;
        float totalLen = totalItems * ItemHeight;
        m_ScrollBar->ViewFraction = (totalLen > 0.0f) ? std::min(1.0f, viewLength / totalLen) : 1.0f;
        if (totalLen > viewLength)
            m_ScrollBar->Value = m_ScrollOffset / (totalLen - viewLength);
        else
            m_ScrollBar->Value = 0.0f;

        // Read back from scroll bar
        float fromBar = m_ScrollBar->Value * std::max(0.0f, totalLen - viewLength);
        if (std::abs(fromBar - m_ScrollOffset) > 0.5f)
            m_ScrollOffset = fromBar;
    }

    // ---- Input ---------------------------------------------------------------

    FReply OnMouseWheel(const Vector2&, float delta) override
    {
        ScrollBy(delta * 60.0f);
        return FReply::Handled();
    }

    // ---- Painting header -----------------------------------------------------

    void PaintHeader(const FPaintContext& ctx, const FGeometry& geom) const
    {
        if (!m_HeaderRow || !ctx.Renderer) return;
        float hh = GetHeaderHeight();
        FGeometry headerGeom = geom.MakeChild(Vector2(0, 0), Vector2(geom.ToRect().w, hh));
        m_HeaderRow->Paint(ctx, headerGeom);
    }

protected:
    float m_ScrollOffset {0.0f};
    std::shared_ptr<SScrollBar> m_ScrollBar;
    std::shared_ptr<SHeaderRow> m_HeaderRow;

    // Selected indices (for multi-select)
    std::set<int32_t> m_SelectedIndices;

    bool m_NeedsRefresh {true};
};

// =============================================================================
// SHeaderRow forward — defined in Views/ (moved from Widgets/ root)
// =============================================================================
// Include-d this header from SHeaderRow.h in Widgets/ directly.
// The canonical location for SHeaderRow is now Views/SHeaderRow.h.

}  // namespace ZSlate
