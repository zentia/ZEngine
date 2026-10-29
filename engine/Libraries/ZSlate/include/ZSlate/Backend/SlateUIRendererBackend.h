#pragma once

#include "ZSlate/Core/SlatePaint.h"
#include "ZSlate/Core/ZSlateTypes.h"

namespace ZSlate
{
// Text measurer backed by the engine's UIRenderer. Plugs into SlateApplication
// so STextBlock can measure text during layout without reaching into the
// renderer's frame state.
class SlateUIRendererTextMeasurer : public ISlateTextMeasurer
{
public:
    // Default constructor (renderer set later via SetRenderer)
    SlateUIRendererTextMeasurer() : m_Renderer(nullptr) {}

    explicit SlateUIRendererTextMeasurer(ISlateRenderer* renderer)
        : m_Renderer(renderer) {}

    void SetRenderer(ISlateRenderer* renderer) { m_Renderer = renderer; }

    Vector2 Measure(const std::string& text, float font_size) const override
    {
        if (m_Renderer)
            return m_Renderer->MeasureText(text, font_size);
        return Vector2(0.0f, font_size);
    }

private:
    ISlateRenderer* m_Renderer;
};
}  // namespace ZSlate
