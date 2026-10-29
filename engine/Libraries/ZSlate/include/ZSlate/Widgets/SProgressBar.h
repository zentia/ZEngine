#pragma once

#include "ZSlate/Widgets/SWidget.h"

namespace ZSlate
{

// =============================================================================
// SProgressBar — progress indicator bar (UE SProgressBar analogue)
// =============================================================================
//
// Simple horizontal/vertical fill bar. 0..1 fraction drives the filled region.
//
class SProgressBar : public SWidget
{
public:
    EOrientation Orientation {EOrientation::Horizontal};

    // Fill fraction 0..1 (0 = empty, 1 = full)
    float Percent {0.0f};

    // Style colours
    UIColor BackgroundColor {0.12f, 0.12f, 0.14f, 1.0f};
    UIColor FillColor       {0.30f, 0.55f, 0.95f, 1.0f};
    UIColor BorderColor     {0.35f, 0.38f, 0.45f, 1.0f};

    // Border inset (pixels)
    float BorderThickness {1.0f};

    Vector2 ComputeDesiredSize() const override
    {
        return Orientation == EOrientation::Horizontal
            ? Vector2(200.0f, 20.0f)
            : Vector2(20.0f, 200.0f);
    }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (!ctx.Renderer) return;
        const UIRect r = geom.ToRect();

        // Background
        ctx.Renderer->DrawQuad(r, BackgroundColor);

        // Fill
        float clamped = std::clamp(Percent, 0.0f, 1.0f);
        float inset = BorderThickness;
        UIRect fillRect;
        if (Orientation == EOrientation::Horizontal)
            fillRect = UIRect(r.x + inset, r.y + inset,
                              (r.w - 2.0f * inset) * clamped, r.h - 2.0f * inset);
        else
            fillRect = UIRect(r.x + inset, r.y + r.h - inset - (r.h - 2.0f * inset) * clamped,
                              r.w - 2.0f * inset, (r.h - 2.0f * inset) * clamped);

        ctx.Renderer->DrawQuad(fillRect, FillColor);

        // Border
        if (BorderThickness > 0.0f)
            ctx.Renderer->DrawRect(r, BorderColor, BorderThickness);
    }
};

}  // namespace ZSlate
