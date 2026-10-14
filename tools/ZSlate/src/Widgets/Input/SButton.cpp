#include "ZSlate/Widgets/Input/SButton.h"

namespace ZSlate
{

Vector2 SButton::ComputeDesiredSize() const
{
    Vector2 desired(Padding.x * 2.0f, Padding.y * 2.0f);
    if (m_Content) desired = desired + m_Content->ComputeDesiredSize();
    return desired;
}

void SButton::OnArrangeChildren(const FGeometry& allottedGeometry,
                                std::vector<FArrangedChild>& outChildren) const
{
    if (!m_Content) return;

    const Vector2 inner(std::max(0.0f, allottedGeometry.LocalSize.x - Padding.x * 2.0f),
                        std::max(0.0f, allottedGeometry.LocalSize.y - Padding.y * 2.0f));

    outChildren.emplace_back(m_Content, allottedGeometry.MakeChild(Padding, inner));
}

void SButton::OnPaint(const FPaintContext& context, const FGeometry& geometry) const
{
    if (!context.Renderer) return;

    const UIRect r = geometry.ToRect();

    UIColor background = NormalColor;
    if (m_Pressed)     background = PressedColor;
    else if (IsHovered()) background = HoverColor;

    context.Renderer->DrawQuad(r, background);
    context.Renderer->DrawRect(r, BorderColor, 1.0f);
}

FReply SButton::OnMouseButtonDown(const Vector2& absolutePos, int button)
{
    (void)absolutePos;
    if (button == 0)
    {
        m_Pressed = true;
        return FReply::Handled();
    }
    return FReply::Unhandled();
}

FReply SButton::OnMouseButtonUp(const Vector2& absolutePos, int button)
{
    (void)absolutePos;
    if (button == 0 && m_Pressed)
    {
        m_Pressed = false;
        if (OnClicked) OnClicked();
        return FReply::Handled();
    }
    m_Pressed = false;
    return FReply::Unhandled();
}

} // namespace ZSlate
