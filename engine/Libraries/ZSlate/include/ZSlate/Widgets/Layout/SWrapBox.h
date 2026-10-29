#pragma once

#include "ZSlate/Widgets/SPanel.h"

namespace ZSlate
{

// =============================================================================
// SWrapBox — flowing layout (UE SWrapBox analogue)
// =============================================================================
//
// Arranges children left-to-right in rows (or top-to-bottom in columns),
// wrapping to the next line when they no longer fit. Uses each child's
// desired size.
//
class SWrapBox : public SPanel
{
public:
    // Flow direction: Horizontal = rows fill left→right then wrap.
    EOrientation Orientation {EOrientation::Horizontal};

    // Gap between children in the flow direction
    float InnerSlotPadding {4.0f};

    // Preferred width (0 = use allotted width). When set, rows wrap at this
    // width rather than the container width — useful for centered wraps.
    float PreferredWidth {0.0f};

    Vector2 ComputeDesiredSize() const override
    {
        Vector2 maxRow {0.0f, 0.0f};
        Vector2 curRow {0.0f, 0.0f};

        for (const auto& slot : m_Slots)
        {
            if (!slot.Content || !slot.Content->IsVisible()) continue;
            Vector2 sz = slot.Content->GetDesiredSize();

            if (Orientation == EOrientation::Horizontal)
            {
                if (curRow.x + sz.x > 4096.0f)  // arbitrary max width for desired-size calc
                {
                    maxRow.x = std::max(maxRow.x, curRow.x);
                    maxRow.y += curRow.y + InnerSlotPadding;
                    curRow = Vector2(sz.x, sz.y);
                }
                else
                {
                    curRow.x += sz.x + (curRow.x > 0.0f ? InnerSlotPadding : 0.0f);
                    curRow.y = std::max(curRow.y, sz.y);
                }
            }
            else
            {
                if (curRow.y + sz.y > 4096.0f)
                {
                    maxRow.x += curRow.x + InnerSlotPadding;
                    maxRow.y = std::max(maxRow.y, curRow.y);
                    curRow = Vector2(sz.x, sz.y);
                }
                else
                {
                    curRow.x = std::max(curRow.x, sz.x);
                    curRow.y += sz.y + (curRow.y > 0.0f ? InnerSlotPadding : 0.0f);
                }
            }
        }
        maxRow.x = std::max(maxRow.x, curRow.x);
        maxRow.y += curRow.y;
        return maxRow;
    }

    void OnArrangeChildren(const FGeometry& AllottedGeometry,
                           std::vector<FArrangedChild>& OutArrangedChildren) const override
    {
        const UIRect view = AllottedGeometry.ToRect();
        float wrapWidth = (PreferredWidth > 0.0f) ? PreferredWidth : view.w;
        float wrapHeight = view.h;

        float cursorX = view.x;
        float cursorY = view.y;
        float maxH = 0.0f;  // max child height in current row
        size_t rowStartIdx = 0;

        auto FlushRow = [&](size_t endIdx)
        {
            // Center the row horizontally within wrapWidth
            float rowW = 0.0f;
            size_t count = 0;
            for (size_t j = rowStartIdx; j < endIdx; ++j)
            {
                if (!m_Slots[j].Content || !m_Slots[j].Content->IsVisible()) continue;
                rowW += m_Slots[j].Content->GetDesiredSize().x;
                ++count;
            }
            if (count > 0) rowW += InnerSlotPadding * (count - 1);
            float rowX = view.x + (wrapWidth - rowW) * 0.5f;

            // Position children in this row
            float cx = rowX;
            for (size_t j = rowStartIdx; j < endIdx; ++j)
            {
                if (!m_Slots[j].Content || !m_Slots[j].Content->IsVisible()) continue;
                Vector2 sz = m_Slots[j].Content->GetDesiredSize();

                FGeometry childGeom = AllottedGeometry.MakeChild(
                    Vector2(cx - view.x, cursorY - view.y), Vector2(sz.x, maxH));
                OutArrangedChildren.emplace_back(m_Slots[j].Content, childGeom);
                cx += sz.x + InnerSlotPadding;
            }
            rowStartIdx = endIdx;
            cursorY += maxH + InnerSlotPadding;
            maxH = 0.0f;
        };

        for (size_t i = 0; i < m_Slots.size(); ++i)
        {
            if (!m_Slots[i].Content || !m_Slots[i].Content->IsVisible()) continue;
            Vector2 sz = m_Slots[i].Content->GetDesiredSize();

            if (Orientation == EOrientation::Horizontal)
            {
                if (cursorX + sz.x > view.x + wrapWidth && cursorX > view.x)
                {
                    FlushRow(i);
                    cursorX = view.x;
                }
                cursorX += sz.x + InnerSlotPadding;
                maxH = std::max(maxH, sz.y);
            }
            else
            {
                if (cursorY + sz.y > view.y + wrapHeight && cursorY > view.y)
                {
                    FlushRow(i);
                    cursorY = view.y;
                }
                cursorY += sz.y + InnerSlotPadding;
                maxH = std::max(maxH, sz.x);
            }
        }
        FlushRow(m_Slots.size());
    }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (!ctx.Renderer) return;
        std::vector<FArrangedChild> arranged;
        OnArrangeChildren(geom, arranged);
        PaintArrangedChildren(ctx, arranged);
    }
};

}  // namespace ZSlate
