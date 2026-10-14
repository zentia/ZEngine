#pragma once

// ============================================================================
// ZSlate/Widgets/Text/STextBlock.h
//
// Read-only single-paragraph label. Assigning `Text`, `FontSize` or `Color`
// takes effect on the next frame — no invalidation needed.
// ============================================================================

#include "../SWidget.h"
#include "../../Core/SlateTextMeasurer.h"
#include "../../Core/ZSlateTypes.h"

#include <string>

namespace ZSlate
{

class STextBlock : public SWidget
{
public:
    std::string  Text;
    float        FontSize = 14.0f;
    UIColor      Color    = Colors::White;
    TextAnchor   Anchor   = TextAnchor::MiddleLeft;
    TextWrapMode Wrap     = TextWrapMode::NoWrap;

    Vector2 ComputeDesiredSize() const override;

    void OnPaint(const FPaintContext& context, const FGeometry& geometry) const override;
};

} // namespace ZSlate
