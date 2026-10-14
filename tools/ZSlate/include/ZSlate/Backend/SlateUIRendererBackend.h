#pragma once

// ============================================================================
// ZSlate/Backend/SlateUIRendererBackend.h
//
// Text measurer backed by the batched renderer (and therefore by the font
// atlas). Installed by the host with SlateApplication::SetTextMeasurer().
// ============================================================================

#include "../Core/SlateTextMeasurer.h"
#include "../Core/ZSlateTypes.h"
#include "../Renderer/ZSlateBatchedRenderer.h"

#include <string>

namespace ZSlate
{

class SlateUIRendererTextMeasurer : public ITextMeasurer
{
public:
    explicit SlateUIRendererTextMeasurer(ZSlateBatchedRenderer* renderer)
        : m_Renderer(renderer) {}

    Vector2 Measure(const std::string& text, float fontSize) const override
    {
        if (m_Renderer) return m_Renderer->MeasureText(text, fontSize);
        // No renderer: rough estimate so layout still works.
        return Vector2(static_cast<float>(Utf8::Length(text)) * fontSize * 0.55f,
                       fontSize * 1.35f);
    }

    ZSlateBatchedRenderer* GetRenderer() const { return m_Renderer; }

private:
    ZSlateBatchedRenderer* m_Renderer = nullptr;
};

} // namespace ZSlate
