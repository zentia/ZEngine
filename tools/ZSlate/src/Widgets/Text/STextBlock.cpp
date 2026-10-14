#include "ZSlate/Widgets/Text/STextBlock.h"

#include <algorithm>

namespace ZSlate
{

Vector2 STextBlock::ComputeDesiredSize() const
{
    const Vector2 measured = MeasureText(Text, FontSize);
    return Vector2(measured.x + 2.0f, std::max(measured.y, FontSize * 1.4f));
}

void STextBlock::OnPaint(const FPaintContext& context, const FGeometry& geometry) const
{
    if (!context.Renderer || Text.empty()) return;

    context.Renderer->DrawTextLabel(geometry.ToRect(), Text, FontSize, Color,
                                    Anchor, Wrap, nullptr);
}

} // namespace ZSlate
