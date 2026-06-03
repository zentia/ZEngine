#pragma once

#include "Runtime/Slate/Widgets/SLeafWidget.h"
#include "Runtime/UI/Render/UIRenderer.h"

#include <string>

namespace ZSlate
{
// A single run of text (UE Slate STextBlock analogue). Measures through the
// installed ISlateTextMeasurer and paints through UIRenderer::drawText.
class STextBlock : public SLeafWidget
{
public:
    std::string Text;
    float FontSize {14.0f};
    UIColor Color {1.0f, 1.0f, 1.0f, 1.0f};
    TextAnchor Alignment {TextAnchor::MiddleLeft};

    void SetText(std::string text) { Text = std::move(text); }

    Vector2 ComputeDesiredSize() const override
    {
        if (GSlateTextMeasurer)
            return GSlateTextMeasurer->Measure(Text, FontSize);
        // Coarse fallback before a measurer is installed.
        return Vector2(static_cast<float>(Text.size()) * FontSize * 0.5f, FontSize * 1.2f);
    }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (ctx.Renderer == nullptr || Text.empty())
            return;
        ctx.Renderer->drawText(geom.ToRect(), Text, FontSize, Color, Alignment, TextWrapMode::NoWrap, nullptr);
    }
};
}  // namespace ZSlate
