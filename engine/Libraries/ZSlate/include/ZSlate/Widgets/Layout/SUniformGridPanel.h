#pragma once

#include "ZSlate/Widgets/SPanel.h"

namespace ZSlate
{

// =============================================================================
// SUniformGridPanel — uniformly-sized grid (UE SUniformGridPanel analogue)
// =============================================================================
//
// Arranges children in a grid where every cell has the same size.
//
class SUniformGridPanel : public SPanel
{
public:
    int32_t NumColumns {2};
    float SlotWidth  {100.0f};
    float SlotHeight {30.0f};

    Vector2 ComputeDesiredSize() const override
    {
        if (m_Slots.empty()) return Vector2(0.0f, 0.0f);
        int32_t numRows = (static_cast<int32_t>(m_Slots.size()) + NumColumns - 1) / NumColumns;
        return Vector2(SlotWidth * NumColumns, SlotHeight * numRows);
    }

    void OnArrangeChildren(const FGeometry& AllottedGeometry,
                           std::vector<FArrangedChild>& OutArrangedChildren) const override
    {
        const UIRect view = AllottedGeometry.ToRect();

        for (size_t i = 0; i < m_Slots.size(); ++i)
        {
            if (!m_Slots[i].Content || !m_Slots[i].Content->IsVisible()) continue;
            int32_t col = static_cast<int32_t>(i) % NumColumns;
            int32_t row = static_cast<int32_t>(i) / NumColumns;

            float sx = view.x + static_cast<float>(col) * SlotWidth;
            float sy = view.y + static_cast<float>(row) * SlotHeight;

            FGeometry childGeom = AllottedGeometry.MakeChild(
                Vector2(sx - view.x, sy - view.y),
                Vector2(SlotWidth, SlotHeight));
            OutArrangedChildren.emplace_back(m_Slots[i].Content, childGeom);
        }
    }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (!ctx.Renderer) return;
        ctx.Renderer->DrawQuad(geom.ToRect(), UIColor(0.08f, 0.08f, 0.10f, 1.0f));

        std::vector<FArrangedChild> arranged;
        OnArrangeChildren(geom, arranged);
        PaintArrangedChildren(ctx, arranged);
    }
};

}  // namespace ZSlate
