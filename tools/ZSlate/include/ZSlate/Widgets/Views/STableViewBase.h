#pragma once

// ============================================================================
// ZSlate/Widgets/Views/STableViewBase.h
//
// Base class for table views that paint their own rows (see SBranchTable).
// It provides the header row, row geometry helpers and hit-testing; the
// derived class supplies the data and the painting.
// ============================================================================

#include "../SWidget.h"
#include "../SHeaderRow.h"
#include "../../Core/SlateTextMeasurer.h"
#include "../../Core/ZSlateTypes.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace ZSlate
{

template<class T>
class STableViewBase : public SWidget
{
public:
    float   RowHeight     = 24.0f;
    int32_t SelectedIndex = -1;

    // Fired by the derived class (or by base input helpers) on row selection.
    std::function<void(const T& item, int32_t index)> OnSelectionChanged;

    void SetHeaderRow(const std::shared_ptr<SHeaderRow>& headerRow) { m_HeaderRow = headerRow; }
    std::shared_ptr<SHeaderRow> GetHeaderRow() const { return m_HeaderRow; }

    // ---- Data (override these) ---------------------------------------------
    virtual int32_t   GetRowCount() const { return 0; }
    virtual const T*  GetItemAt(int32_t index) const { (void)index; return nullptr; }

    // ---- Geometry ----------------------------------------------------------
    float GetHeaderHeight() const
    {
        return m_HeaderRow ? m_HeaderRow->ComputeDesiredSize().y : 0.0f;
    }

    // Absolute position -> row index, or -1 when outside the body.
    int32_t HitTestRow(const Vector2& position, const UIRect& bounds) const
    {
        const float headerH = GetHeaderHeight();
        const float rel     = position.y - bounds.y - headerH + m_TableScrollOffset;
        if (rel < 0.0f) return -1;
        if (RowHeight <= 0.0f) return -1;
        const int32_t index = static_cast<int32_t>(rel / RowHeight);
        return (index >= 0 && index < GetRowCount()) ? index : -1;
    }

    bool WantsFillHeight() const override { return true; }

    Vector2 ComputeDesiredSize() const override
    {
        Vector2 size(0.0f, GetHeaderHeight());
        if (m_HeaderRow)
        {
            for (const auto& column : m_HeaderRow->Columns) size.x += column.Width;
        }
        size.y += RowHeight * static_cast<float>(std::max(0, GetRowCount()));
        return size;
    }

    void OnArrangeChildren(const FGeometry& allottedGeometry,
                           std::vector<FArrangedChild>& outChildren) const override
    {
        // Derived classes paint everything themselves; nothing to arrange.
        (void)allottedGeometry; (void)outChildren;
    }

protected:
    // Alternating row shading with a highlight for the selected row. Derived
    // tables call this once per visible row before drawing the cells.
    void PaintRowBackground(const FPaintContext& context,
                            const UIRect& rowRect,
                            int32_t rowIndex) const
    {
        if (!context.Renderer) return;

        UIColor background = (rowIndex % 2 == 0)
            ? UIColor(0.16f, 0.18f, 0.22f, 1.0f)
            : UIColor(0.13f, 0.15f, 0.19f, 1.0f);

        if (rowIndex == SelectedIndex)
        {
            background = UIColor(0.25f, 0.50f, 0.90f, 0.35f);
        }

        context.Renderer->DrawQuad(rowRect, background);
    }

    std::shared_ptr<SHeaderRow> m_HeaderRow;
    mutable float               m_TableScrollOffset = 0.0f;
};

} // namespace ZSlate
