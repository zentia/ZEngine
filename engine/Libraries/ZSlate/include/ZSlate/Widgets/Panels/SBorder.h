#pragma once

// SBorder: a single-child container that paints a background (and optional
// outline) behind its padded content.
// Directory: Widgets/Panels/SBorder.h  (UE analogue: Widgets/Layout/SBorder.h)

#include "ZSlate/Core/SlateGeometry.h"
#include "ZSlate/Widgets/SCompoundWidget.h"

namespace ZSlate
{

class SBorder : public SCompoundWidget
{
public:
    SBorder() = default;
    virtual ~SBorder() = default;

    void SetBorder(const FMargin& m) { (void)m; }
    void SetPadding(const FMargin& p) { Padding = p; }
    void SetBorderColor(const UIColor& c) { BorderColor = c; BorderThickness = std::max(BorderThickness, 1.0f); }
    void SetBackgroundColor(const UIColor& c) { BackgroundColor = c; }

    // Padding / HAlign / VAlign come from SCompoundWidget (content fills by default).
    UIColor   BackgroundColor {0.2f, 0.2f, 0.2f, 1.0f};
    bool      DrawBackground {true};
    UIColor   BorderColor {0.5f, 0.5f, 0.5f, 1.0f};
    float     BorderThickness {0.0f};   // 0 = no outline

    // The content is painted by SWidget::Paint through ArrangeChildren; this
    // only draws the border's own visuals.
    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (!ctx.Renderer)
            return;
        const UIRect rect = geom.ToRect();
        if (DrawBackground)
            ctx.Renderer->DrawQuad(rect, BackgroundColor);
        if (BorderThickness > 0.0f)
            ctx.Renderer->DrawRect(rect, BorderColor, BorderThickness);
    }
};

}  // namespace ZSlate
