#include "ZSlate/Widgets/SHeaderRow.h"

#include <algorithm>
#include <cmath>

namespace ZSlate
{

// ---------------------------------------------------------------------------
// Layout helpers
// ---------------------------------------------------------------------------

int SHeaderRow::FindColumn(const std::string& name) const
{
    for (size_t i = 0; i < Columns.size(); ++i)
        if (Columns[i].Name == name) return static_cast<int>(i);
    return -1;
}

void SHeaderRow::ApplyLayout(const std::vector<std::pair<std::string, float>>& orderAndWidth)
{
    std::vector<FColumn> ordered;
    ordered.reserve(Columns.size());

    for (const auto& entry : orderAndWidth)
    {
        const int index = FindColumn(entry.first);
        if (index < 0) continue;

        FColumn column = Columns[index];
        column.Width = std::max(column.MinWidth, entry.second);
        ordered.push_back(column);
    }

    // Anything the layout never mentions keeps its width and goes to the end.
    for (const FColumn& column : Columns)
    {
        bool present = false;
        for (const auto& entry : orderAndWidth)
            if (entry.first == column.Name) { present = true; break; }
        if (!present) ordered.push_back(column);
    }

    Columns = std::move(ordered);
}

int SHeaderRow::HitTestBoundary(float localX) const
{
    float x = 0.0f;
    for (size_t i = 0; i < Columns.size(); ++i)
    {
        x += Columns[i].Width;
        if (std::fabs(localX - x) <= GrabWidth * 0.5f) return static_cast<int>(i);
    }
    return -1;
}

int SHeaderRow::HitTestColumn(float localX) const
{
    float x = 0.0f;
    for (size_t i = 0; i < Columns.size(); ++i)
    {
        const float width = Columns[i].Width;
        if (localX >= x && localX < x + width) return static_cast<int>(i);
        x += width;
    }
    return (localX >= 0.0f && localX < x) ? static_cast<int>(Columns.size()) - 1 : -1;
}

float SHeaderRow::BoundaryOffset(int insertionIndex) const
{
    float x = 0.0f;
    for (int i = 0; i < insertionIndex && i < static_cast<int>(Columns.size()); ++i)
        x += Columns[i].Width;
    return x;
}

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------

void SHeaderRow::OnPaint(const FPaintContext& context, const FGeometry& geometry) const
{
    if (!context.Renderer) return;

    const UIRect r = geometry.ToRect();
    context.Renderer->DrawQuad(r, BackgroundColor);

    float x = r.x;
    for (size_t i = 0; i < Columns.size(); ++i)
    {
        const FColumn& column = Columns[i];
        const int      index  = static_cast<int>(i);

        const bool isDragged = (m_DragMode == EDragMode::Reorder && index == m_ReorderFrom);
        if (isDragged)
            context.Renderer->DrawQuad(UIRect(x, r.y, column.Width, r.h), DraggingColor);
        else if (index == m_HoverColumn)
            context.Renderer->DrawQuad(UIRect(x, r.y, column.Width, r.h), HoverColor);

        // A 28px gutter is reserved for a future sort arrow.
        const UIRect textRect(x + 6.0f, r.y, std::max(0.0f, column.Width - 34.0f), r.h);
        context.Renderer->DrawTextLabel(textRect, column.Name, FontSize, TextColor,
                                        TextAnchor::MiddleLeft, TextWrapMode::NoWrap, nullptr);

        const bool grabbable = (m_DragMode == EDragMode::Resize && index == m_ResizeIndex)
                            || (m_DragMode == EDragMode::None && index == m_HoverBoundary);
        const float dividerW = grabbable ? 2.0f : 1.0f;
        const UIRect divider(x + column.Width - dividerW, r.y, dividerW, r.h);
        context.Renderer->DrawQuad(divider, grabbable ? DropIndicatorColor : DividerColor);

        x += column.Width;
    }

    // Where the column would land if the drag ended now.
    if (m_DragMode == EDragMode::Reorder && m_ReorderFrom >= 0 && m_ReorderTo != m_ReorderFrom)
    {
        const float dropX = r.x + BoundaryOffset(m_ReorderTo > m_ReorderFrom
                                                     ? m_ReorderTo + 1
                                                     : m_ReorderTo);
        const UIRect indicator(dropX - 1.0f, r.y, 2.0f, r.h);
        context.Renderer->DrawQuad(indicator, DropIndicatorColor);
    }

    context.Renderer->DrawRect(r, DividerColor, 1.0f);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

FReply SHeaderRow::OnMouseButtonDown(const Vector2& absolutePos, int button)
{
    if (button != 0) return FReply::Unhandled();

    const UIRect r       = GetCachedGeometry().ToRect();
    const float  localX  = absolutePos.x - r.x;

    if (Resizable)
    {
        const int boundary = HitTestBoundary(localX);
        if (boundary >= 0)
        {
            m_DragMode          = EDragMode::Resize;
            m_ResizeIndex       = boundary;
            m_ResizeStartX      = absolutePos.x;
            m_ResizeStartWidth  = Columns[boundary].Width;
            return FReply::Handled();
        }
    }

    if (Reorderable)
    {
        const int index = HitTestColumn(localX);
        if (index >= 0)
        {
            m_DragMode     = EDragMode::Reorder;
            m_ReorderFrom  = index;
            m_ReorderTo    = index;
            m_PressX       = absolutePos.x;
            return FReply::Handled();
        }
    }

    return FReply::Unhandled();
}

FReply SHeaderRow::OnMouseMove(const Vector2& absolutePos)
{
    const UIRect r      = GetCachedGeometry().ToRect();
    const float  localX = absolutePos.x - r.x;

    m_HoverBoundary = Resizable   ? HitTestBoundary(localX) : -1;
    m_HoverColumn   = (m_DragMode == EDragMode::None) ? HitTestColumn(localX) : m_HoverColumn;

    if (m_DragMode == EDragMode::Resize &&
        m_ResizeIndex >= 0 && m_ResizeIndex < static_cast<int>(Columns.size()))
    {
        FColumn& column = Columns[m_ResizeIndex];
        column.Width = std::max(column.MinWidth,
                                m_ResizeStartWidth + (absolutePos.x - m_ResizeStartX));
        return FReply::Handled();
    }

    if (m_DragMode == EDragMode::Reorder && m_ReorderFrom >= 0)
    {
        if (std::fabs(absolutePos.x - m_PressX) >= DragThreshold)
        {
            const int target = HitTestColumn(localX);
            if (target >= 0) m_ReorderTo = target;
        }
        return FReply::Handled();
    }

    return FReply::Unhandled();
}

FReply SHeaderRow::OnMouseButtonUp(const Vector2& absolutePos, int button)
{
    (void)absolutePos;
    if (button != 0 || m_DragMode == EDragMode::None) return FReply::Unhandled();

    bool changed = false;

    if (m_DragMode == EDragMode::Resize)
    {
        changed = (m_ResizeIndex >= 0);
    }
    else if (m_DragMode == EDragMode::Reorder &&
             m_ReorderFrom >= 0 && m_ReorderTo >= 0 &&
             m_ReorderFrom != m_ReorderTo &&
             m_ReorderFrom < static_cast<int>(Columns.size()))
    {
        FColumn moved = Columns[m_ReorderFrom];
        Columns.erase(Columns.begin() + m_ReorderFrom);

        int insertAt = (m_ReorderTo > m_ReorderFrom) ? m_ReorderTo - 1 : m_ReorderTo;
        insertAt = std::clamp(insertAt, 0, static_cast<int>(Columns.size()));

        Columns.insert(Columns.begin() + insertAt, std::move(moved));
        changed = true;
    }

    m_DragMode    = EDragMode::None;
    m_ResizeIndex = -1;
    m_ReorderFrom = -1;
    m_ReorderTo   = -1;

    if (changed && OnColumnsChanged) OnColumnsChanged();

    return FReply::Handled();
}

} // namespace ZSlate
