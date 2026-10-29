#pragma once

#include "ZSlate/Widgets/SCompoundWidget.h"

namespace ZSlate
{

// =============================================================================
// SScaleBox — scale child to fit (UE SScaleBox analogue)
// =============================================================================
//
// Scales its single child to fit the allotted space while preserving
// aspect ratio. Stretch mode: Fit, Fill, FillWidth, FillHeight.
//
enum class EScaleMode { Fit, Fill, FillWidth, FillHeight };

class SScaleBox : public SCompoundWidget
{
public:
    EScaleMode ScaleMode {EScaleMode::Fit};

    // Child content
    void SetContent(std::shared_ptr<SWidget> widget) { m_Content = std::move(widget); }
    std::shared_ptr<SWidget> GetContent() const { return m_Content; }

    Vector2 ComputeDesiredSize() const override
    {
        return m_Content ? m_Content->ComputeDesiredSize() : Vector2(0, 0);
    }

    void ArrangeChildren(const FGeometry& AllottedGeometry,
                         std::vector<FArrangedWidget>& OutArrangedChildren) const override
    {
        if (!m_Content) return;

        Vector2 childSize = m_Content->ComputeDesiredSize();
        const UIRect view = AllottedGeometry.ToRect();

        if (childSize.x <= 0 || childSize.y <= 0)
        {
            OutArrangedChildren.emplace_back(m_Content, AllottedGeometry);
            return;
        }

        float scaleX = view.w / childSize.x;
        float scaleY = view.h / childSize.y;
        float scale;

        switch (ScaleMode)
        {
        case EScaleMode::Fit:
            scale = std::min(scaleX, scaleY);
            break;
        case EScaleMode::Fill:
            scale = std::max(scaleX, scaleY);
            break;
        case EScaleMode::FillWidth:
            scale = scaleX;
            break;
        case EScaleMode::FillHeight:
            scale = scaleY;
            break;
        default:
            scale = 1.0f;
        }

        float sw = childSize.x * scale;
        float sh = childSize.y * scale;
        float ox = view.x + (view.w - sw) * 0.5f;
        float oy = view.y + (view.h - sh) * 0.5f;

        FGeometry childGeom = AllottedGeometry.MakeChild(
            Vector2(ox - view.x, oy - view.y), Vector2(sw, sh));
        OutArrangedChildren.emplace_back(m_Content, childGeom);
    }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        // SCompoundWidget handles painting via ArrangeChildren + child paint
        SCompoundWidget::OnPaint(ctx, geom);
    }

private:
    std::shared_ptr<SWidget> m_Content;
};

}  // namespace ZSlate
