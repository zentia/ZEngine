#pragma once

#include "ZSlate/Application/SlateInput.h"
#include "ZSlate/Widgets/Views/STableViewBase.h"
#include "ZSlate/Widgets/Views/STableRow.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

namespace ZSlate
{

// =============================================================================
// TSimpleListViewDataSource<T> — shared item storage for list views
// =============================================================================
//
// Owns the items and a revision counter. Any number of SListView<T> instances
// bound through SListView::SetDataSource() pick up a change on their next
// layout / paint / input pass (they compare revisions), so host code mutates
// the data source and never has to poke the view.
//
template <typename T>
class TSimpleListViewDataSource
{
public:
    TSimpleListViewDataSource() = default;
    explicit TSimpleListViewDataSource(std::vector<T> items) : m_Items(std::move(items)) {}

    void SetItems(std::vector<T> items) { m_Items = std::move(items); ++m_Revision; }
    std::vector<T> GetItems() const { return m_Items; }
    const std::vector<T>& GetItemsRef() const { return m_Items; }

    // Bumped on every mutation; views use it to detect staleness.
    uint64_t GetRevision() const { return m_Revision; }
    void MarkDirty() { ++m_Revision; }

private:
    std::vector<T> m_Items;
    uint64_t       m_Revision {1};
};

// =============================================================================
// SListView<T> — vertical list view with widget recycling (UE SListView analogue)
// =============================================================================
//
// Inherits from STableViewBase for scroll/selection/scrollbar/header.
// Only creates widgets for visible items; drops them when they scroll out of
// view (index -> widget map).
//
// Items come either from the public `Items` vector (call RegenerateAllWidgets
// after mutating it) or from a bound TSimpleListViewDataSource (automatic).
//
template <typename ItemType>
class SListView : public STableViewBase
{
public:
    using FOnGenerateRow = std::function<std::shared_ptr<SWidget>(const ItemType&, int32_t)>;
    using FOnGenerateItemWidget =
        std::function<std::shared_ptr<SWidget>(const ItemType&, int32_t, const std::shared_ptr<SListView<ItemType>>&)>;

    FOnGenerateRow OnGenerateRow;
    std::function<void(const ItemType&, int32_t)> OnSelectionChanged;

    // Fired on the second press of a double-click over a row. The row is
    // selected first; the header strip forwards to SHeaderRow instead.
    std::function<void(const ItemType&, int32_t)> OnItemDoubleClicked;

    // Fired on right-button press over a row (the row is selected first).
    // Position is in the same space as the routed mouse events.
    std::function<void(int32_t, const Vector2&)> OnItemRightClicked;

    // Data source
    std::vector<ItemType> Items;

    // Styling (alpha 0 = not drawn)
    UIColor BackgroundColor         {0.10f, 0.11f, 0.13f, 1.0f};
    UIColor BorderColor             {0.25f, 0.27f, 0.32f, 1.0f};
    UIColor SelectedBackgroundColor {0.18f, 0.35f, 0.62f, 1.0f};
    UIColor HoverBackgroundColor    {0.17f, 0.19f, 0.23f, 1.0f};

    // ---- Data source binding -------------------------------------------------

    void SetDataSource(std::shared_ptr<TSimpleListViewDataSource<ItemType>> dataSource)
    {
        m_DataSource = std::move(dataSource);
        m_DataRevision = 0;
        SyncDataSource();
    }
    std::shared_ptr<TSimpleListViewDataSource<ItemType>> GetDataSource() const { return m_DataSource; }

    // ---- Callback setters (alternative to assigning the public members) -------

    template <class F>
    void SetOnGenerateItemWidget(F&& callback)
    {
        m_OnGenerateItemWidget = FOnGenerateItemWidget(std::forward<F>(callback));
        RegenerateAllWidgets();
    }
    void SetOnSelectionChanged(std::function<void(const std::vector<int32_t>&)> callback)
    {
        m_OnSelectionIndicesChanged = std::move(callback);
    }
    void SetOnItemRightClicked(std::function<void(int32_t, const Vector2&)> callback)
    {
        OnItemRightClicked = std::move(callback);
    }
    void SetOnItemDoubleClicked(std::function<void(int32_t)> callback)
    {
        m_OnItemIndexDoubleClicked = std::move(callback);
    }

    // ---- Selection ---------------------------------------------------------

    // Programmatic (de)selection; does not fire the selection callbacks.
    // In single-select mode selecting a row clears the others.
    void SetSelection(int32_t index, bool selected)
    {
        if (selected && SelectionMode != ESelectionMode::Multi)
            m_SelectedIndices.clear();
        SetSelected(index, selected);
    }

    // Selected indices in ascending order.
    const std::vector<int32_t>& GetSelectedIndices() const
    {
        m_SelectedIndicesCache.assign(m_SelectedIndices.begin(), m_SelectedIndices.end());
        return m_SelectedIndicesCache;
    }
    int32_t GetSelectedIndex() const
    {
        return m_SelectedIndices.empty() ? -1 : *m_SelectedIndices.begin();
    }

    // ---- Layout / paint ------------------------------------------------------

    Vector2 ComputeDesiredSize() const override
    {
        SyncDataSource();
        float w = 0.0f;
        if (m_HeaderRow)
        {
            w = m_HeaderRow->ComputeDesiredSize().x;
        }
        return Vector2(std::max(w, 200.0f),
                       Items.size() * ItemHeight + GetHeaderHeight());
    }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (!ctx.Renderer) return;
        SyncDataSource();

        const UIRect view = geom.ToRect();
        if (BackgroundColor.w > 0.0f) ctx.Renderer->DrawQuad(view, BackgroundColor);
        if (BorderColor.w > 0.0f) ctx.Renderer->DrawRect(view, BorderColor, 1.0f);

        float hh = GetHeaderHeight();

        // Header
        PaintHeader(ctx, geom);

        if (Items.empty() || ItemHeight <= 0.0f) return;

        // Content area (clipped)
        float contentY = view.y + hh;
        float contentH = view.h - hh;
        ctx.Renderer->PushClipRect(UIRect(view.x, contentY, view.w, contentH));

        // Sync scroll before deciding which rows are visible
        const_cast<SListView*>(this)->ClampScrollOffset(static_cast<int32_t>(Items.size()), contentH);
        const_cast<SListView*>(this)->SyncScrollBar(static_cast<int32_t>(Items.size()), contentH);

        // Visible range
        int32_t first = std::max(0, static_cast<int32_t>(m_ScrollOffset / ItemHeight));
        int32_t last = std::min(static_cast<int32_t>(Items.size()),
                                first + static_cast<int32_t>(contentH / ItemHeight) + 2);
        // +2 for partial visibility at top/bottom

        for (int32_t i = first; i < last; ++i)
        {
            float y = contentY + i * ItemHeight - m_ScrollOffset;
            UIRect rowRect(view.x, y, view.w, ItemHeight);

            if (IsSelected(i))
                ctx.Renderer->DrawQuad(rowRect, SelectedBackgroundColor);
            else if (i == m_HoveredIndex && HoverBackgroundColor.w > 0.0f)
                ctx.Renderer->DrawQuad(rowRect, HoverBackgroundColor);

            auto rowWidget = GetOrCreateWidget(i);
            if (rowWidget)
            {
                FGeometry rowGeom = geom.MakeChild(
                    Vector2(0.0f, y - view.y), Vector2(view.w, ItemHeight));
                // Desired sizes must be cached before Paint: the widget was
                // just built and its children have never been measured, so
                // any Auto slot inside the row would arrange at size 0.
                rowWidget->CacheDesiredSize();
                rowWidget->Paint(ctx, rowGeom);
            }
        }

        // Drop the widgets that scrolled out of view. Without this the
        // item->widget map grows for every row ever visited, so scrolling a
        // large list retains a widget per row until the view is destroyed.
        ReleaseInvisibleWidgets(first, last - 1);

        ctx.Renderer->PopClipRect();
    }

    // ---- Input ---------------------------------------------------------------

    void OnMouseMove(const Vector2& pos) override
    {
        m_HoveredIndex = HitTestRow(pos);
    }

    void OnMouseLeave() override { m_HoveredIndex = -1; }

    FReply OnMouseButtonDown(const Vector2& pos, int btn) override
    {
        SyncDataSource();
        const UIRect view = GetCachedGeometry().ToRect();
        const float  hh   = GetHeaderHeight();

        // Header click (delegate to header; it hit-tests against its own
        // cached geometry, so no geometry needs to be passed here)
        if (pos.y < view.y + hh)
            return (m_HeaderRow && btn == 0) ? m_HeaderRow->OnMouseButtonDown(pos, btn) : FReply::Unhandled();

        const int32_t idx = HitTestRow(pos);

        if (btn == 1)  // right click: select the row, then let the host open a context menu
        {
            if (idx < 0) return FReply::Unhandled();
            if (!IsSelected(idx)) SelectExclusive(idx);
            if (OnItemRightClicked) OnItemRightClicked(idx, pos);
            return FReply::Handled();
        }
        if (btn == 2) return FReply::Handled();
        if (btn != 0) return FReply::Unhandled();

        if (idx >= 0)
        {
            // Ctrl toggles in multi-select mode; otherwise a click selects
            // exactly the clicked row (UE Slate semantics).
            if (SelectionMode == ESelectionMode::Multi && SlateInputRouter::IsCtrlDown())
            {
                SetSelected(idx, !IsSelected(idx));
                NotifySelectionChanged(idx);
            }
            else
            {
                SelectExclusive(idx);
            }
            m_LastClickedIndex = idx;
            return FReply::Handled();
        }
        return FReply::Unhandled();
    }

    FReply OnMouseButtonUp(const Vector2& pos, int btn) override
    {
        const UIRect view = GetCachedGeometry().ToRect();
        if (m_HeaderRow && btn == 0 && pos.y < view.y + GetHeaderHeight())
            return m_HeaderRow->OnMouseButtonUp(pos, btn);
        return FReply::Unhandled();
    }

    FReply OnMouseButtonDoubleClick(const Vector2& pos, int btn) override
    {
        SyncDataSource();
        if (btn != 0 || Items.empty()) return FReply::Unhandled();

        const UIRect view = GetCachedGeometry().ToRect();
        const float  hh   = GetHeaderHeight();

        if (pos.y < view.y + hh)
        {
            return m_HeaderRow ? m_HeaderRow->OnMouseButtonDoubleClick(pos, btn)
                               : FReply::Unhandled();
        }

        const int32_t idx = HitTestRow(pos);
        if (idx < 0) return FReply::Unhandled();

        if (!IsSelected(idx) || GetSelectedCount() != 1) SelectExclusive(idx);
        m_LastClickedIndex = idx;

        if (OnItemDoubleClicked)
            OnItemDoubleClicked(Items[idx], idx);
        if (m_OnItemIndexDoubleClicked)
            m_OnItemIndexDoubleClicked(idx);
        return FReply::Handled();
    }

    // ---- Widget recycling ----------------------------------------------------

    // Call after mutating Items so cached rows are rebuilt from the new data.
    // The cache is keyed by index, so stale entries would otherwise keep
    // rendering the previous contents of those indices.
    void RegenerateAllWidgets()
    {
        m_ItemToWidget.clear();
        RequestRefresh();
    }

    // ---- Diagnostics -------------------------------------------------------

    // Row widgets currently alive. Virtualisation is only real if this stays
    // proportional to the *visible* row count rather than to Items.size();
    // tests/slate_list_view_tests.cpp asserts exactly that at 180K items.
    size_t GetLiveWidgetCount() const { return m_ItemToWidget.size(); }

private:
    mutable int32_t m_HoveredIndex {-1};
    mutable int32_t m_LastClickedIndex {-1};

    std::shared_ptr<TSimpleListViewDataSource<ItemType>> m_DataSource;
    mutable uint64_t m_DataRevision {0};

    FOnGenerateItemWidget m_OnGenerateItemWidget;
    std::function<void(const std::vector<int32_t>&)> m_OnSelectionIndicesChanged;
    std::function<void(int32_t)> m_OnItemIndexDoubleClicked;
    mutable std::vector<int32_t> m_SelectedIndicesCache;

    // Index -> row widget, holding only the rows currently on screen.
    //
    // There is deliberately no free-list of spare widgets here. OnGenerateRow
    // is the only way to bind a row to its data, and SWidget exposes no "rebind
    // to this item" hook, so a widget taken from such a pool would keep
    // rendering the item it was built for - i.e. scrolling would show the wrong
    // values in the right place, which is worse than reallocating. Rows are
    // therefore built on demand and dropped once off screen; the cost is bounded
    // by the number of visible rows, not by the size of Items.
    mutable std::unordered_map<int32_t, std::shared_ptr<SWidget>> m_ItemToWidget;

    // Pull new items from the bound data source when its revision changed.
    void SyncDataSource() const
    {
        if (!m_DataSource || m_DataSource->GetRevision() == m_DataRevision) return;
        auto* self = const_cast<SListView*>(this);
        self->Items = m_DataSource->GetItemsRef();
        m_DataRevision = m_DataSource->GetRevision();

        // Drop selections that no longer point at an item.
        const int32_t count = static_cast<int32_t>(Items.size());
        for (auto it = self->m_SelectedIndices.begin(); it != self->m_SelectedIndices.end();)
            it = (*it >= count) ? self->m_SelectedIndices.erase(it) : std::next(it);

        self->RegenerateAllWidgets();
    }

    int32_t HitTestRow(const Vector2& pos) const
    {
        if (ItemHeight <= 0.0f) return -1;
        const UIRect view = GetCachedGeometry().ToRect();
        const float  top  = view.y + GetHeaderHeight();
        if (pos.y < top || pos.y >= view.Bottom() || pos.x < view.x || pos.x >= view.Right()) return -1;
        const int32_t idx = static_cast<int32_t>((pos.y - top + m_ScrollOffset) / ItemHeight);
        return (idx >= 0 && idx < static_cast<int32_t>(Items.size())) ? idx : -1;
    }

    void SelectExclusive(int32_t idx)
    {
        m_SelectedIndices.clear();
        m_SelectedIndices.insert(idx);
        NotifySelectionChanged(idx);
    }

    void NotifySelectionChanged(int32_t idx)
    {
        if (OnSelectionChanged && idx >= 0 && idx < static_cast<int32_t>(Items.size()))
            OnSelectionChanged(Items[idx], idx);
        if (m_OnSelectionIndicesChanged)
            m_OnSelectionIndicesChanged(GetSelectedIndices());
    }

    std::shared_ptr<SWidget> GetOrCreateWidget(int32_t idx) const
    {
        auto it = m_ItemToWidget.find(idx);
        if (it != m_ItemToWidget.end())
            return it->second;

        std::shared_ptr<SWidget> widget;
        if (OnGenerateRow)
            widget = OnGenerateRow(Items[idx], idx);
        else if (m_OnGenerateItemWidget)
        {
            auto self = std::static_pointer_cast<SListView<ItemType>>(
                std::const_pointer_cast<SWidget>(shared_from_this()));
            widget = m_OnGenerateItemWidget(Items[idx], idx, self);
        }

        if (widget)
            m_ItemToWidget[idx] = widget;
        return widget;
    }

    // Forget every cached row outside [firstVisible, lastVisible].
    void ReleaseInvisibleWidgets(int32_t firstVisible, int32_t lastVisible) const
    {
        for (auto it = m_ItemToWidget.begin(); it != m_ItemToWidget.end();)
        {
            if (it->first < firstVisible || it->first > lastVisible)
                it = m_ItemToWidget.erase(it);
            else
                ++it;
        }
    }
};

}  // namespace ZSlate
