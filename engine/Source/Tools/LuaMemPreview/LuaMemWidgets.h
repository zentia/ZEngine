#pragma once

// ============================================================================
// LuaMemWidgets - custom-drawn ZSlate widgets for the Lua memory previewer
// ============================================================================
//
// Why these are hand-painted leaf widgets rather than SListView rows:
//
// A snapshot holds ~180K rows. SListView<T> builds one child SWidget per
// visible row through OnGenerateRow, but its recycling path is broken in two
// ways that make it unusable at this scale:
//   * GetOrCreateWidget() only calls OnGenerateRow when the pool is empty, so a
//     widget pulled from the pool keeps the previous row's data - scrolling
//     would show wrong values.
//   * ReleaseInvisibleWidgets() is never called from OnPaint, so the
//     item->widget map grows monotonically; scrolling the whole list would
//     retain 180K widgets.
// Painting the rows directly also means zero per-row allocations and a cost
// that depends only on how many rows actually fit on screen.

#include "LuaMemCsv.h"

#include "ZSlate/Widgets/SLeafWidget.h"

#include <functional>
#include <string>
#include <vector>

namespace LuaMemPreview
{
// Shared palette so the bar, the chips and the table's type column agree.
ZSlate::UIColor ColorForType(const std::string& type_name);

class SLuaTypeBar;

// Paints the "what does this bucket count" card for the currently hovered
// chip, clamped inside 'screen'. Call this AFTER the widget hierarchy has been
// painted - see SLuaTypeBar::GetHoveredChip for why it cannot live in the bar.
// No-op when nothing is hovered.
void PaintTypeDocCard(ZSlate::ISlateRenderer* r, const ZSlate::UIRect& screen,
                      const SLuaTypeBar& bar, const FSnapshot& snapshot);

// ----------------------------------------------------------------------------
// SLuaTypeBar - stacked proportion bar + clickable type chips
//
// This is the part that actually answers "which category owns the memory".
// With 180K rows whose largest single entry is ~2 MB, scrolling can never
// surface that; the per-type split has to be shown up front.
// ----------------------------------------------------------------------------
class SLuaTypeBar : public ZSlate::SLeafWidget
{
public:
    const FSnapshot* Snapshot {nullptr};
    const FRowView* View {nullptr};

    // Called with the clicked type id, or FRowView::kAllTypes for the total chip.
    std::function<void(uint16_t)> OnTypeClicked;

    ZSlate::Vector2 ComputeDesiredSize() const override;
    void OnPaint(const ZSlate::FPaintContext& ctx, const ZSlate::FGeometry& geom) const override;
    ZSlate::FReply OnMouseButtonDown(const ZSlate::Vector2& pos, int button) override;
    void OnMouseMove(const ZSlate::Vector2& pos) override;
    void OnMouseLeave() override;
    ZSlate::ECursorType GetCursor() const override;

    // Hovered chip, for the explanation card. The card cannot be painted by
    // this widget: the bar is an auto-sized slot in a vertical box and the
    // table is painted after it, so anything overflowing the bar's own
    // geometry gets covered. The host paints it after the whole hierarchy.
    struct FHoverInfo
    {
        bool Valid {false};
        uint16_t TypeId {0};
        ZSlate::UIRect ChipRect;  // absolute, for anchoring the card
    };
    FHoverInfo GetHoveredChip() const;

private:
    struct FChipHit
    {
        ZSlate::UIRect Rect;
        uint16_t TypeId;
    };

    // Chip rects are produced while painting (that is when the width is known)
    // and reused for hit-testing on the next event.
    mutable std::vector<FChipHit> m_ChipHits;
    int m_HoverChip {-1};
};

// ----------------------------------------------------------------------------
// SLuaMemTable - virtualised, sortable table painted in one widget
// ----------------------------------------------------------------------------
class SLuaMemTable : public ZSlate::SLeafWidget
{
public:
    const FSnapshot* Snapshot {nullptr};
    FRowView* View {nullptr};

    // Fired when the selected row changes (row index into FSnapshot::GetRows,
    // or -1 when the selection was cleared).
    std::function<void(int64_t)> OnSelectionChanged;
    // Fired when the user clicks a header and the sort changed.
    std::function<void()> OnSortChanged;

    ZSlate::Vector2 ComputeDesiredSize() const override;
    void OnPaint(const ZSlate::FPaintContext& ctx, const ZSlate::FGeometry& geom) const override;

    ZSlate::FReply OnMouseButtonDown(const ZSlate::Vector2& pos, int button) override;
    ZSlate::FReply OnMouseButtonUp(const ZSlate::Vector2& pos, int button) override;
    void OnMouseMove(const ZSlate::Vector2& pos) override;
    void OnMouseLeave() override;
    ZSlate::FReply OnMouseWheel(const ZSlate::Vector2& pos, float delta) override;
    bool SupportsKeyboardFocus() const override { return true; }
    ZSlate::FReply OnKeyDown(ZSlate::EKey key) override;
    ZSlate::ECursorType GetCursor() const override;

    // Scroll by whole pages; the host maps PageUp/PageDown onto this because
    // ZSlate's EKey has no page keys.
    void ScrollByPage(int direction);
    // Drop the selection and scroll back to the top (after a filter change).
    void ResetView();

    int64_t GetSelectedRow() const { return m_SelectedRow; }

private:
    // Index into FRowView::GetIndices() of the row at `local_y` pixels inside
    // the content area, or -1.
    int64_t ViewIndexAtY(float local_y, const ZSlate::UIRect& view) const;
    void SelectViewIndex(int64_t view_index);
    void ClampScroll(float content_height) const;
    float TotalContentHeight() const;

    // Column x-offsets resolved for the current width.
    struct FLayout
    {
        float CountW, SizeW, TypeW, StackX, StackW, ScrollbarX, ScrollbarW;
    };
    FLayout ComputeLayout(const ZSlate::UIRect& view) const;

    mutable float m_ScrollOffset {0.0f};
    mutable float m_ViewportHeight {0.0f};
    int64_t m_SelectedRow {-1};       // row index into the snapshot
    int64_t m_SelectedViewIndex {-1}; // position within the current view
    int64_t m_HoverViewIndex {-1};

    // Scrollbar drag state.
    bool m_DraggingScrollbar {false};
    float m_DragGrabOffset {0.0f};
    mutable ZSlate::UIRect m_ThumbRect {0, 0, 0, 0};
    bool m_HoverHeader {false};
};

// ----------------------------------------------------------------------------
// SLuaStackDetail - full multi-line stack of the selected row
// ----------------------------------------------------------------------------
class SLuaStackDetail : public ZSlate::SLeafWidget
{
public:
    const FSnapshot* Snapshot {nullptr};

    void SetRow(int64_t row_index);

    ZSlate::Vector2 ComputeDesiredSize() const override;
    void OnPaint(const ZSlate::FPaintContext& ctx, const ZSlate::FGeometry& geom) const override;
    ZSlate::FReply OnMouseWheel(const ZSlate::Vector2& pos, float delta) override;

private:
    int64_t m_Row {-1};
    std::string m_Header;              // "proto · 1.46 MB · 3.7K objects"
    std::vector<std::string> m_Lines;  // stack split per line
    mutable float m_ScrollOffset {0.0f};
};

}  // namespace LuaMemPreview
