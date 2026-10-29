#pragma once

#include "ZSlate/Widgets/SCompoundWidget.h"

#include <string>

namespace ZSlate
{

// =============================================================================
// STooltip — hover tooltip (UE STooltip analogue)
// =============================================================================
//
// A floating text label that appears after a short delay when hovering
// over a widget. The host widget calls Show/Hide.
//
class STooltip : public SCompoundWidget
{
public:
    std::string Text {};

    // Styling
    float FontSize {12.0f};
    float PaddingX {8.0f};
    float PaddingY {4.0f};

    // Auto-sizing based on text content
    Vector2 ComputeDesiredSize() const override
    {
        if (Text.empty()) return Vector2(0, 0);
        float approxW = static_cast<float>(Text.size()) * FontSize * 0.55f + PaddingX * 2.0f;
        float h = FontSize + PaddingY * 2.0f;
        return Vector2(approxW, h);
    }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (!ctx.Renderer || Text.empty()) return;
        const UIRect r = geom.ToRect();

        // Background
        ctx.Renderer->DrawQuad(r, UIColor(0.05f, 0.05f, 0.08f, 0.95f));
        ctx.Renderer->DrawRect(r, UIColor(0.30f, 0.32f, 0.38f, 1.0f), 1.0f);

        // Text
        ctx.Renderer->DrawTextLabel(
            UIRect(r.x + PaddingX, r.y + PaddingY, r.w - PaddingX * 2.0f, r.h - PaddingY * 2.0f),
            Text, FontSize,
            UIColor(0.85f, 0.88f, 0.94f, 1.0f),
            TextAnchor::MiddleLeft, TextWrapMode::NoWrap);
    }
};

// =============================================================================
// ITooltipProvider — interface for widgets that show tooltips
// =============================================================================

struct ITooltipProvider
{
    virtual ~ITooltipProvider() = default;
    virtual std::string GetTooltipText() const = 0;
};

}  // namespace ZSlate
