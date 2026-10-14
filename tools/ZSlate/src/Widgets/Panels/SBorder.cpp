#include "ZSlate/Widgets/Panels/SBorder.h"

#include <algorithm>

namespace ZSlate
{

Vector2 SBorder::ComputeDesiredSize() const
{
    Vector2 result(Padding.x * 2.0f, Padding.y * 2.0f);
    if (m_Content)
    {
        const Vector2 desired = m_Content->ComputeDesiredSize();
        result.x += desired.x;
        result.y += desired.y;
    }
    return result;
}

void SBorder::OnArrangeChildren(const FGeometry& allottedGeometry,
                                std::vector<FArrangedChild>& outChildren) const
{
    if (!m_Content) return;

    const Vector2 inner(allottedGeometry.LocalSize.x - Padding.x * 2.0f,
                        allottedGeometry.LocalSize.y - Padding.y * 2.0f);

    outChildren.emplace_back(m_Content,
        allottedGeometry.MakeChild(Padding, Vector2(std::max(0.0f, inner.x),
                                                    std::max(0.0f, inner.y))));
}

void SBorder::OnPaint(const FPaintContext& context, const FGeometry& geometry) const
{
    if (!context.Renderer) return;

    const UIRect r = geometry.ToRect();
    context.Renderer->DrawQuad(r, BackgroundColor);
    if (BorderThickness > 0.0f)
    {
        context.Renderer->DrawRect(r, BorderColor, BorderThickness);
    }
}

} // namespace ZSlate
