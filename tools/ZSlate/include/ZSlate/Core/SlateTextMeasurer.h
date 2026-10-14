#pragma once

// ============================================================================
// ZSlate/Core/SlateTextMeasurer.h
//
// Text measurement is injected by the host (the concrete implementation lives
// in Backend/SlateUIRendererBackend.h and uses the batched renderer + font
// atlas). Widgets fall back to a cheap estimate when nothing is installed.
// ============================================================================

#include "ZSlateTypes.h"

#include <string>

namespace ZSlate
{

class ITextMeasurer
{
public:
    virtual ~ITextMeasurer() = default;
    virtual Vector2 Measure(const std::string& text, float fontSize) const = 0;
};

// Measures with the measurer installed on SlateApplication. When none is
// installed a cheap per-code-point estimate is returned so layout never
// collapses to zero.
Vector2 MeasureText(const std::string& text, float fontSize);

} // namespace ZSlate
