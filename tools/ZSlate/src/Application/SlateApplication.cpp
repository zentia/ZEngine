#include "ZSlate/Application/SlateApplication.h"

#include "ZSlate/Widgets/SWidget.h"

#include <algorithm>

namespace ZSlate
{

SlateApplication& SlateApplication::Get()
{
    static SlateApplication instance;
    return instance;
}

void SlateApplication::PaintInto(ISlateRenderer* renderer, const UIRect& region)
{
    if (!renderer || !m_Root) return;

    FPaintContext context;
    context.Renderer = renderer;

    const FGeometry rootGeometry(Vector2(region.x, region.y),
                                 Vector2(region.w, region.h));

    PaintWidget(m_Root, context, rootGeometry);

    // ---- modal layer -------------------------------------------------------
    if (!m_Modal) return;

    renderer->DrawQuad(region, UIColor(0.0f, 0.0f, 0.0f, 0.55f));

    const Vector2 desired = m_Modal->ComputeDesiredSize();
    const float   maxW = std::max(240.0f, region.w - 40.0f);
    const float   maxH = std::max(120.0f, region.h - 40.0f);
    const float   w = std::clamp(desired.x, 240.0f, maxW);
    const float   h = std::clamp(desired.y, 120.0f, maxH);
    const float   x = region.x + (region.w - w) * 0.5f;
    const float   y = region.y + (region.h - h) * 0.35f;

    PaintWidget(m_Modal, context, FGeometry(Vector2(x, y), Vector2(w, h)));
}

Vector2 MeasureText(const std::string& text, float fontSize)
{
    ITextMeasurer* measurer = SlateApplication::Get().GetTextMeasurer();
    if (measurer) return measurer->Measure(text, fontSize);

    // Fallback estimate so layout still produces sane sizes.
    return Vector2(static_cast<float>(Utf8::Length(text)) * fontSize * 0.55f,
                   fontSize * 1.35f);
}

} // namespace ZSlate
