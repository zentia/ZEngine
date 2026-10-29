#pragma once

#include "ZSlate/Widgets/Views/ITableRow.h"
#include "ZSlate/Widgets/SCompoundWidget.h"

#include <functional>
#include <memory>

namespace ZSlate
{

// =============================================================================
// STableRow<T> — reusable row widget for list/tree views (UE STableRow analogue)
// =============================================================================

enum class ETableRowSignalSelectionMode
{
    Instant,   // OnMouseButtonDown
    Deferred,  // OnMouseButtonUp
};

enum class EItemDropZone
{
    AboveItem,
    BelowItem,
    OntoItem,
    None,
};

template <typename ItemType>
class STableRow : public SCompoundWidget, public ITableRow
{
public:
    using FOnGenerateWidget = std::function<std::shared_ptr<SWidget>(const ItemType&, int32_t)>;

    FOnGenerateWidget OnGenerateWidget;
    float RowHeight {24.0f};

    // ---- ITableRow -----------------------------------------------------------

    bool IsSelected() const override { return m_Selected; }
    void SetSelected(bool s) override { m_Selected = s; }

    // ---- Widget overrides ----------------------------------------------------

    Vector2 ComputeDesiredSize() const override { return Vector2(200.0f, RowHeight); }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (!ctx.Renderer) return;
        const UIRect r = geom.ToRect();

        // Selection & hover background
        if (m_Selected)
            ctx.Renderer->DrawQuad(r, UIColor(0.18f, 0.35f, 0.62f, 1.0f));
        else if (m_Hovered)
            ctx.Renderer->DrawQuad(r, UIColor(0.14f, 0.14f, 0.18f, 1.0f));

        // Content widget
        if (m_Content)
        {
            FGeometry childGeom = geom.MakeChild(Vector2(4.0f, 0.0f), Vector2(r.w - 8.0f, r.h));
            m_Content->Paint(ctx, childGeom);
        }

        // Bottom separator
        ctx.Renderer->DrawRect(
            UIRect(r.x, r.Bottom() - 1.0f, r.w, 1.0f),
            UIColor(0.08f, 0.08f, 0.10f, 1.0f), 1.0f);
    }

    // ---- Input ---------------------------------------------------------------

    void OnMouseEnter() override { m_Hovered = true; }
    void OnMouseLeave() override { m_Hovered = false; }

    FReply OnMouseButtonDown(const Vector2&, int btn) override
    {
        if (btn != 0) return FReply::Unhandled();
        m_Selected = !m_Selected;
        return FReply::Handled();
    }

    // ---- Data binding --------------------------------------------------------

    void BindItem(const ItemType& item, int32_t index)
    {
        m_ItemIndex = index;
        if (OnGenerateWidget)
            m_Content = OnGenerateWidget(item, index);
    }

    void ClearContent() { m_Content.reset(); }

    int32_t GetItemIndex() const { return m_ItemIndex; }

private:
    mutable bool m_Hovered {false};
    mutable bool m_Selected {false};
    std::shared_ptr<SWidget> m_Content;
    int32_t m_ItemIndex {0};
};

}  // namespace ZSlate
