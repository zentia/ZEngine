#include "ZSlate/Widgets/Input/SCheckBox.h"

#include <algorithm>

namespace ZSlate
{

void SCheckBox::SetChecked(bool checked, bool notify)
{
    if (Checked == checked) return;
    Checked = checked;
    if (notify && OnCheckStateChanged) OnCheckStateChanged(Checked);
}

Vector2 SCheckBox::ComputeDesiredSize() const
{
    const float box      = FontSize * 1.15f;
    const float gap      = 6.0f;
    const float textW    = Label.empty() ? 0.0f : MeasureText(Label, FontSize).x;
    return Vector2(box + gap + textW, std::max(box, FontSize * 1.4f));
}

void SCheckBox::OnPaint(const FPaintContext& context, const FGeometry& geometry) const
{
    if (!context.Renderer) return;

    const UIRect r   = geometry.ToRect();
    const float  box = FontSize * 1.15f;
    const float  y   = r.y + (r.h - box) * 0.5f;

    const UIRect boxRect(r.x, y, box, box);
    context.Renderer->DrawQuad(boxRect, BoxBackgroundColor);
    context.Renderer->DrawRect(boxRect, BoxBorderColor, 1.0f);

    if (Checked)
    {
        // Filled inner square — cheap and readable at any DPI.
        context.Renderer->DrawQuad(boxRect.InsetBy(box * 0.22f), CheckColor);
    }

    if (!Label.empty())
    {
        const UIRect textRect(r.x + box + 6.0f, r.y,
                              std::max(0.0f, r.w - box - 6.0f), r.h);
        context.Renderer->DrawTextLabel(textRect, Label, FontSize, TextColor,
                                        TextAnchor::MiddleLeft, TextWrapMode::NoWrap, nullptr);
    }
}

FReply SCheckBox::OnMouseButtonUp(const Vector2& absolutePos, int button)
{
    (void)absolutePos;
    if (button != 0) return FReply::Unhandled();

    Checked = !Checked;
    if (OnCheckStateChanged) OnCheckStateChanged(Checked);
    return FReply::Handled();
}

} // namespace ZSlate
