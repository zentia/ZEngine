#pragma once

// ============================================================================
// ZSlate/Widgets/SHeaderRow.h
//
// Column header strip. `Columns` is a plain aggregate so it can be written as
//   header->Columns = { {"Drive", 70.0f, 50.0f}, {"Name", 180.0f, 120.0f} };
//
// Interaction (new)
//  * Drag the boundary between two columns to resize the column on its left.
//  * Drag a column header sideways to reorder it.
//  * Both finish by firing OnColumnsChanged, which is where host code persists
//    the layout (the C# app writes grid_layout.json from that same moment).
// ============================================================================

#include "ZSlate/Widgets/SWidget.h"
#include "ZSlate/Core/ZSlateTypes.h"
#include "ZSlate/Core/SlateTextMeasurer.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ZSlate
{

class SHeaderRow : public SWidget
{
public:
    struct FColumn
    {
        std::string Name;
        float       Width;
        float       MinWidth = 40.0f;
    };

    std::vector<FColumn> Columns;

    float   RowHeight = 28.0f;
    float   FontSize  = 13.0f;

    UIColor BackgroundColor    { 0.16f, 0.18f, 0.23f, 1.0f };
    UIColor TextColor          { 0.88f, 0.90f, 0.96f, 1.0f };
    UIColor DividerColor       { 0.05f, 0.05f, 0.07f, 1.0f };
    UIColor HoverColor         { 0.21f, 0.24f, 0.30f, 1.0f };
    UIColor DraggingColor      { 0.26f, 0.31f, 0.40f, 1.0f };
    UIColor DropIndicatorColor { 0.35f, 0.65f, 1.0f, 1.0f };

    // ---- Interaction -------------------------------------------------------
    bool Resizable   = true;
    bool Reorderable = true;

    // Hit zone (pixels) around each boundary that starts a resize.
    float GrabWidth = 6.0f;
    // Horizontal travel before a press on a header turns into a column drag.
    float DragThreshold = 4.0f;

    // Fired once, after a resize or a reorder is committed.
    std::function<void()> OnColumnsChanged;

    int FindColumn(const std::string& name) const;   // -1 when absent

    // Re-applies a persisted layout (column name -> width, in display order).
    // Columns the layout does not mention keep their width and are appended in
    // their original relative order.
    void ApplyLayout(const std::vector<std::pair<std::string, float>>& orderAndWidth);

    float GetTotalWidth() const
    {
        float total = 0.0f;
        for (const auto& column : Columns) total += column.Width;
        return total;
    }

    Vector2 ComputeDesiredSize() const override
    {
        return Vector2(GetTotalWidth(), RowHeight);
    }

    void OnPaint(const FPaintContext& context, const FGeometry& geometry) const override;

    FReply OnMouseButtonDown(const Vector2& absolutePos, int button) override;
    FReply OnMouseMove      (const Vector2& absolutePos)             override;
    FReply OnMouseButtonUp  (const Vector2& absolutePos, int button) override;

private:
    enum class EDragMode { None, Resize, Reorder };

    int   HitTestBoundary(float localX) const;   // index of the column to resize
    int   HitTestColumn  (float localX) const;   // index of the column under x
    float BoundaryOffset (int insertionIndex) const;

    mutable EDragMode m_DragMode     = EDragMode::None;
    mutable int       m_HoverColumn  = -1;
    mutable int       m_HoverBoundary = -1;

    int   m_ResizeIndex       = -1;
    float m_ResizeStartX      = 0.0f;
    float m_ResizeStartWidth  = 0.0f;

    int   m_ReorderFrom  = -1;
    int   m_ReorderTo    = -1;
    float m_PressX       = 0.0f;
};

} // namespace ZSlate
