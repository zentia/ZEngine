#pragma once

#include "ZSlate/Widgets/SPanel.h"

namespace ZSlate
{

// =============================================================================
// SGridPanel — uniform grid layout (UE SGridPanel analogue)
// =============================================================================
//
// Arranges children in a regular N×M grid. Column widths = max child width
// per column; row heights = max child height per row. Empty cells are skipped.
//
// Usage:
//   auto grid = std::make_shared<SGridPanel>();
//   grid->SetColumns(3);
//   grid->AddSlot() = button1;
//   grid->AddSlot() = button2;
//   grid->AddSlot() = button3;  // row 1
//   grid->AddSlot() = button4;  // row 2, col 1
//
class SGridPanel : public SPanel
{
public:
    // Number of columns. Children are filled left-to-right, top-to-bottom.
    int32_t NumColumns {2};

    // Gap between cells (pixels)
    float CellPadding {4.0f};

    void SetColumns(int32_t cols) { NumColumns = std::max(1, cols); }

    Vector2 ComputeDesiredSize() const override
    {
        if (m_Slots.empty()) return Vector2(0.0f, 0.0f);

        int32_t numRows = (static_cast<int32_t>(m_Slots.size()) + NumColumns - 1) / NumColumns;
        std::vector<float> colWidths(NumColumns, 0.0f);
        std::vector<float> rowHeights(numRows, 0.0f);

        for (size_t i = 0; i < m_Slots.size(); ++i)
        {
            if (!m_Slots[i].Content || !m_Slots[i].Content->IsVisible()) continue;
            Vector2 sz = m_Slots[i].Content->ComputeDesiredSize();
            int32_t col = static_cast<int32_t>(i) % NumColumns;
            int32_t row = static_cast<int32_t>(i) / NumColumns;
            colWidths[col] = std::max(colWidths[col], sz.x);
            rowHeights[row] = std::max(rowHeights[row], sz.y);
        }

        float totalW = 0.0f, totalH = 0.0f;
        for (float w : colWidths) totalW += w;
        for (float h : rowHeights) totalH += h;
        totalW += CellPadding * (NumColumns - 1);
        totalH += CellPadding * (numRows - 1);

        return Vector2(totalW, totalH);
    }

    void OnArrangeChildren(const FGeometry& AllottedGeometry,
                           std::vector<FArrangedChild>& OutArrangedChildren) const override
    {
        if (m_Slots.empty()) return;

        int32_t numRows = (static_cast<int32_t>(m_Slots.size()) + NumColumns - 1) / NumColumns;
        std::vector<float> colWidths(NumColumns, 0.0f);
        std::vector<float> rowHeights(numRows, 0.0f);

        // Measure all cells
        for (size_t i = 0; i < m_Slots.size(); ++i)
        {
            if (!m_Slots[i].Content || !m_Slots[i].Content->IsVisible()) continue;
            Vector2 sz = m_Slots[i].Content->ComputeDesiredSize();
            int32_t col = static_cast<int32_t>(i) % NumColumns;
            int32_t row = static_cast<int32_t>(i) / NumColumns;
            colWidths[col] = std::max(colWidths[col], sz.x);
            rowHeights[row] = std::max(rowHeights[row], sz.y);
        }

        // Compute starting offset (center the grid in the allotted space)
        float totalW = 0.0f, totalH = 0.0f;
        for (float w : colWidths) totalW += w;
        for (float h : rowHeights) totalH += h;
        totalW += CellPadding * (NumColumns - 1);
        totalH += CellPadding * (numRows - 1);

        const UIRect view = AllottedGeometry.ToRect();
        float startX = view.x + (view.w - totalW) * 0.5f;
        float startY = view.y + (view.h - totalH) * 0.5f;

        // Position children
        for (size_t i = 0; i < m_Slots.size(); ++i)
        {
            if (!m_Slots[i].Content || !m_Slots[i].Content->IsVisible()) continue;

            int32_t col = static_cast<int32_t>(i) % NumColumns;
            int32_t row = static_cast<int32_t>(i) / NumColumns;

            float cellX = startX;
            for (int32_t c = 0; c < col; ++c) cellX += colWidths[c] + CellPadding;
            float cellY = startY;
            for (int32_t r = 0; r < row; ++r) cellY += rowHeights[r] + CellPadding;

            FGeometry childGeom = AllottedGeometry.MakeChild(
                Vector2(cellX - view.x, cellY - view.y),
                Vector2(colWidths[col], rowHeights[row]));

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
