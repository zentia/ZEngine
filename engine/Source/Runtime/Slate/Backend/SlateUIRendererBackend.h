#pragma once

#include "Runtime/Slate/Core/SlatePaint.h"
#include "Runtime/UI/Render/UIRenderer.h"

#include <string>

namespace ZSlate
{
// Runtime text measurer: forwards ISlateTextMeasurer::Measure to a live
// UIRenderer (BatchedUIRenderer in player builds). This replaces the editor-only
// ImGui measurer (SlateImGuiTextMeasurer) so ZSlate layout works in the runtime
// where ImGui is not available.
//
// Measurement is single-line (NoWrap) because ComputeDesiredSize() asks for the
// intrinsic size of a text run; wrapping is resolved later by the widget's
// arranged geometry.
class SlateUIRendererTextMeasurer final : public ISlateTextMeasurer
{
public:
    SlateUIRendererTextMeasurer() = default;
    explicit SlateUIRendererTextMeasurer(UIRenderer* renderer) : m_Renderer(renderer) {}

    void SetRenderer(UIRenderer* renderer) { m_Renderer = renderer; }
    UIRenderer* GetRenderer() const { return m_Renderer; }

    Vector2 Measure(const std::string& text, float font_size) const override
    {
        if (m_Renderer == nullptr)
        {
            // Coarse fallback (same heuristic STextBlock uses when no measurer is
            // installed) so layout still produces sane sizes before first frame.
            return Vector2(static_cast<float>(text.size()) * font_size * 0.5f, font_size);
        }
        return m_Renderer->measureText(text, font_size, TextWrapMode::NoWrap, 0.0f, nullptr);
    }

private:
    UIRenderer* m_Renderer {nullptr};
};
}  // namespace ZSlate
