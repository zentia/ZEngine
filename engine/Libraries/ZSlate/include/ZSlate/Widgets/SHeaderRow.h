#pragma once

#include "ZSlate/Widgets/SWidget.h"
#include "ZSlate/Widgets/SPanel.h"

#include <algorithm>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ZSlate
{

// =============================================================================
// SHeaderRow — table column headers (UE SHeaderRow analogue)
// =============================================================================
//
// A horizontal bar of resizable column headers. Each column has
// a label, a width, and an optional sort arrow.
//
class SHeaderRow : public SPanel
{
public:
    enum class EColumnSortMode { None, Ascending, Descending };

    struct FColumn
    {
        std::string Label;
        float Width {100.0f};
        float MinWidth {30.0f};
        EColumnSortMode SortMode {EColumnSortMode::None};
        std::function<void()> OnSort;
    };

    std::vector<FColumn> Columns;

    float RowHeight {24.0f};
    float FontSize {12.0f};

    // ---- Reordering + persistence -------------------------------------------
    // Resizing is built in; dragging a header sideways reorders it. Both finish
    // by firing OnColumnsChanged, which is where host code persists the layout.
    bool  Reorderable {true};
    float DragThreshold {4.0f};   // horizontal travel before a press becomes a drag
    std::function<void()> OnColumnsChanged;

    int32_t FindColumn(const std::string& label) const
    {
        for (int32_t i = 0; i < static_cast<int32_t>(Columns.size()); ++i)
            if (Columns[i].Label == label) return i;
        return -1;
    }

    // Re-applies a persisted layout (label -> width, in display order). Columns
    // the layout does not mention keep their width and are appended in their
    // original relative order.
    void ApplyLayout(const std::vector<std::pair<std::string, float>>& orderAndWidth)
    {
        std::vector<FColumn> ordered;
        ordered.reserve(Columns.size());

        for (const auto& entry : orderAndWidth)
        {
            const int32_t index = FindColumn(entry.first);
            if (index < 0) continue;
            FColumn column = Columns[index];
            column.Width = std::max(column.MinWidth, entry.second);
            ordered.push_back(column);
        }

        for (const FColumn& column : Columns)
        {
            bool present = false;
            for (const auto& entry : orderAndWidth)
                if (entry.first == column.Label) { present = true; break; }
            if (!present) ordered.push_back(column);
        }

        Columns = std::move(ordered);
    }

    // ---- Interaction state --------------------------------------------------
    mutable int32_t ResizingColumn {-1};
    mutable float   ResizeStartX {0.0f};
    mutable float   ResizeStartWidth {0.0f};

    mutable int32_t ReorderFrom {-1};
    mutable int32_t ReorderTo   {-1};
    mutable float   ReorderStartX {0.0f};

    int32_t HitTestColumn(float localX) const
    {
        float cursorX = 0.0f;
        for (int32_t i = 0; i < static_cast<int32_t>(Columns.size()); ++i)
        {
            if (localX >= cursorX && localX < cursorX + Columns[i].Width) return i;
            cursorX += Columns[i].Width;
        }
        return -1;
    }

    Vector2 ComputeDesiredSize() const override
    {
        float totalW = 0.0f;
        for (const auto& col : Columns) totalW += col.Width;
        return Vector2(totalW, RowHeight);
    }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (!ctx.Renderer) return;
        const UIRect r = geom.ToRect();

        float cursorX = r.x;
        for (size_t i = 0; i < Columns.size(); ++i)
        {
            const auto& col = Columns[i];
            UIRect cr(cursorX, r.y, col.Width, RowHeight);

            // Background
            ctx.Renderer->DrawQuad(cr, UIColor(0.10f, 0.10f, 0.12f, 1.0f));
            ctx.Renderer->DrawRect(cr, UIColor(0.20f, 0.22f, 0.28f, 1.0f), 1.0f);

            // Label
            ctx.Renderer->DrawTextLabel(
                UIRect(cr.x + 6.0f, cr.y, cr.w - 28.0f, cr.h),
                col.Label, FontSize,
                UIColor(0.80f, 0.82f, 0.88f, 1.0f),
                TextAnchor::MiddleLeft, TextWrapMode::NoWrap);

            // Sort arrow
            if (col.SortMode != EColumnSortMode::None)
            {
                const char* arrow = (col.SortMode == EColumnSortMode::Ascending) ? "\xE2\x96\xB2" : "\xE2\x96\xBC";
                ctx.Renderer->DrawTextLabel(
                    UIRect(cr.Right() - 24.0f, cr.y, 18.0f, cr.h),
                    arrow, FontSize - 2.0f,
                    UIColor(0.60f, 0.65f, 0.75f, 1.0f),
                    TextAnchor::MiddleCenter, TextWrapMode::NoWrap);
            }

            // Resize handle (right edge)
            if (i < Columns.size() - 1)
            {
                float handleX = cr.Right() - 2.0f;
                float handleW = (static_cast<int32_t>(i) == ResizingColumn) ? 2.0f : 4.0f;
                ctx.Renderer->DrawQuad(
                    UIRect(handleX, cr.y, handleW, cr.h),
                    (static_cast<int32_t>(i) == ResizingColumn)
                        ? UIColor(0.5f, 0.6f, 1.0f, 0.8f)
                        : UIColor(0.25f, 0.27f, 0.33f, 1.0f));
            }

            cursorX += col.Width;
        }

        // Where the dragged column would land if the drag ended now.
        if (ReorderFrom >= 0 && ReorderTo != ReorderFrom)
        {
            const int32_t boundary = (ReorderTo > ReorderFrom) ? ReorderTo + 1 : ReorderTo;
            float x = r.x;
            for (int32_t i = 0; i < boundary && i < static_cast<int32_t>(Columns.size()); ++i)
                x += Columns[i].Width;
            ctx.Renderer->DrawQuad(UIRect(x - 1.0f, r.y, 2.0f, RowHeight),
                                   UIColor(0.5f, 0.7f, 1.0f, 0.9f));
        }
    }

    // ---- Input ---------------------------------------------------------------

    void OnMouseMove(const Vector2& pos) override
    {
        if (ResizingColumn >= 0)
        {
            float delta = pos.x - ResizeStartX;
            float newW = std::max(Columns[ResizingColumn].MinWidth, ResizeStartWidth + delta);
            Columns[ResizingColumn].Width = newW;
            return;
        }

        // A press on a header turns into a reorder drag once it travels far
        // enough sideways.
        if (Reorderable && ReorderFrom >= 0 &&
            std::abs(pos.x - ReorderStartX) >= DragThreshold)
        {
            const UIRect r = GetCachedGeometry().ToRect();
            const int32_t target = HitTestColumn(pos.x - r.x);
            if (target >= 0) ReorderTo = target;
        }
    }

    FReply OnMouseButtonDown(const Vector2& pos, int btn) override
    {
        if (btn != 0) return FReply::Unhandled();

        const UIRect r = GetCachedGeometry().ToRect();
        float cursorX = r.x;

        for (int32_t i = 0; i < static_cast<int32_t>(Columns.size()) - 1; ++i)
        {
            cursorX += Columns[i].Width;

            // Resize hit test (within 4px of column right edge)
            if (std::abs(pos.x - cursorX) < 4.0f)
            {
                ResizingColumn = i;
                ResizeStartX = pos.x;
                ResizeStartWidth = Columns[i].Width;
                return FReply::Handled().CaptureMouse(const_cast<SHeaderRow*>(this));
            }
        }

        // Pressed a header: remember it so a drag can reorder it. Sorting is
        // decided on the button-up edge (UE does the same) — a press that turns
        // into a drag must not also toggle the sort.
        const int32_t hit = HitTestColumn(pos.x - r.x);
        if (hit >= 0)
        {
            ReorderFrom   = hit;
            ReorderTo     = hit;
            ReorderStartX = pos.x;
            return FReply::Handled().CaptureMouse(const_cast<SHeaderRow*>(this));
        }
        return FReply::Unhandled();
    }

    FReply OnMouseButtonUp(const Vector2&, int btn) override
    {
        if (btn != 0) return FReply::Unhandled();

        bool changed = false;

        if (ResizingColumn >= 0)
        {
            ResizingColumn = -1;
            changed = true;
        }
        else if (ReorderFrom >= 0 && ReorderFrom < static_cast<int32_t>(Columns.size()))
        {
            const int32_t from = ReorderFrom;
            const int32_t to   = ReorderTo;

            if (Reorderable && to >= 0 && to != from)
            {
                FColumn moved = Columns[from];
                Columns.erase(Columns.begin() + from);

                int32_t insertAt = (to > from) ? to - 1 : to;
                insertAt = std::clamp(insertAt, 0, static_cast<int32_t>(Columns.size()));
                Columns.insert(Columns.begin() + insertAt, std::move(moved));
                changed = true;
            }
            else if (Columns[from].OnSort)
            {
                // No drag: treat it as a click on the column (sort cycle).
                switch (Columns[from].SortMode)
                {
                case EColumnSortMode::None:    Columns[from].SortMode = EColumnSortMode::Ascending; break;
                case EColumnSortMode::Ascending: Columns[from].SortMode = EColumnSortMode::Descending; break;
                case EColumnSortMode::Descending: Columns[from].SortMode = EColumnSortMode::None; break;
                }
                // Clear other columns
                for (int32_t j = 0; j < static_cast<int32_t>(Columns.size()); ++j)
                    if (j != from) Columns[j].SortMode = EColumnSortMode::None;
                Columns[from].OnSort();
            }

            ReorderFrom = -1;
            ReorderTo   = -1;
        }
        else
        {
            return FReply::Unhandled();
        }

        if (changed && OnColumnsChanged) OnColumnsChanged();
        return FReply::Handled().ReleaseMouseCapture();
    }

    void OnMouseCaptureLost() override
    {
        ResizingColumn = -1;
        ReorderFrom    = -1;
        ReorderTo      = -1;
    }
};

}  // namespace ZSlate
