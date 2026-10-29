#pragma once

#include "ZSlate/Widgets/Views/STableViewBase.h"
#include "ZSlate/Widgets/Views/STableRow.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

namespace ZSlate
{

// =============================================================================
// STileView<T> — grid/tile item view (UE STileView analogue)
// =============================================================================
//
// Arranges items in a uniform grid of tiles. Inherits from STableViewBase
// for scroll/selection. Item width drives column count from viewport width.
//
template <typename ItemType>
class STileView : public STableViewBase
{
public:
    using FOnGenerateTile = std::function<std::shared_ptr<SWidget>(const ItemType&, int32_t)>;

    FOnGenerateTile OnGenerateTile;
    std::function<void(const ItemType&, int32_t)> OnSelectionChanged;

    // Tile dimensions
    float TileWidth  {100.0f};
    float TileHeight {100.0f};

    // Data
    std::vector<ItemType> Items;

    Vector2 ComputeDesiredSize() const override
    {
        int32_t cols = std::max(1, static_cast<int32_t>(400.0f / TileWidth));
        int32_t rows = (static_cast<int32_t>(Items.size()) + cols - 1) / cols;
        return Vector2(cols * TileWidth, rows * TileHeight + GetHeaderHeight());
    }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (!ctx.Renderer || Items.empty()) return;
        const UIRect view = geom.ToRect();
        float hh = GetHeaderHeight();

        // Header
        PaintHeader(ctx, geom);

        // Content area
        float contentY = view.y + hh;
        float contentH = view.h - hh;

        ctx.Renderer->PushClipRect(UIRect(view.x, contentY, view.w, contentH));

        int32_t cols = std::max(1, static_cast<int32_t>(view.w / TileWidth));
        int32_t firstRow = static_cast<int32_t>(m_ScrollOffset / TileHeight);
        int32_t lastRow = static_cast<int32_t>((m_ScrollOffset + contentH) / TileHeight) + 1;

        for (int32_t row = firstRow; row <= lastRow; ++row)
        {
            for (int32_t col = 0; col < cols; ++col)
            {
                int32_t idx = row * cols + col;
                if (idx < 0 || idx >= static_cast<int32_t>(Items.size())) continue;

                float tx = view.x + col * TileWidth;
                float ty = contentY + row * TileHeight - m_ScrollOffset;
                UIRect tileRect(tx, ty, TileWidth, TileHeight);

                // Selection highlight
                if (IsSelected(idx))
                    ctx.Renderer->DrawQuad(tileRect, UIColor(0.18f, 0.35f, 0.62f, 1.0f));

                // Generate widget for this tile
                if (OnGenerateTile)
                {
                    auto tile = OnGenerateTile(Items[idx], idx);
                    if (tile)
                    {
                        FGeometry tileGeom = geom.MakeChild(
                            Vector2(tx - view.x, ty - view.y),
                            Vector2(TileWidth, TileHeight));
                        tile->Paint(ctx, tileGeom);
                    }
                }
            }
        }

        // Sync scroll
        int32_t rows = (static_cast<int32_t>(Items.size()) + cols - 1) / cols;
        ClampScrollOffset(rows, contentH / TileHeight);
        SyncScrollBar(rows, contentH / TileHeight);

        ctx.Renderer->PopClipRect();
    }

    // ---- Input ---------------------------------------------------------------

    FReply OnMouseButtonDown(const Vector2& pos, int btn) override
    {
        if (btn != 0 || Items.empty()) return FReply::Unhandled();

        const UIRect view = GetCachedGeometry().ToRect();
        float hh = GetHeaderHeight();

        int32_t cols = std::max(1, static_cast<int32_t>(view.w / TileWidth));
        int32_t col = static_cast<int32_t>((pos.x - view.x) / TileWidth);
        int32_t row = static_cast<int32_t>((pos.y - view.y - hh + m_ScrollOffset) / TileHeight);
        int32_t idx = row * cols + col;

        if (idx >= 0 && idx < static_cast<int32_t>(Items.size()))
        {
            bool nowSelected = !IsSelected(idx);
            SetSelected(idx, nowSelected);
            if (OnSelectionChanged)
                OnSelectionChanged(Items[idx], idx);
            return FReply::Handled();
        }
        return STableViewBase::OnMouseButtonDown(pos, btn);
    }
};

}  // namespace ZSlate
