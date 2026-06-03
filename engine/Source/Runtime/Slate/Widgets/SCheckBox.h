#pragma once

#include "Runtime/Slate/Widgets/SLeafWidget.h"

#include <functional>

namespace ZSlate
{
// A square toggle (UE Slate SCheckBox analogue). Renders a box with a filled
// inner mark when checked; toggles on click and reports via OnCheckStateChanged.
class SCheckBox : public SLeafWidget
{
public:
    bool Checked {false};
    float BoxSize {18.0f};
    UIColor BorderColor {0.45f, 0.46f, 0.50f, 1.0f};
    UIColor BackgroundColor {0.16f, 0.16f, 0.19f, 1.0f};
    UIColor HoverColor {0.22f, 0.22f, 0.26f, 1.0f};
    UIColor CheckColor {0.36f, 0.62f, 0.96f, 1.0f};

    std::function<void(bool)> OnCheckStateChanged;

    Vector2 ComputeDesiredSize() const override { return Vector2(BoxSize, BoxSize); }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (ctx.Renderer == nullptr)
            return;
        const UIRect rect = geom.ToRect();
        ctx.Renderer->drawQuad(rect, m_Hovered ? HoverColor : BackgroundColor);
        ctx.Renderer->drawRect(rect, BorderColor, 1.0f);
        if (Checked)
        {
            const float inset = rect.width * 0.25f;
            const UIRect mark(rect.x + inset, rect.y + inset, rect.width - inset * 2.0f, rect.height - inset * 2.0f);
            ctx.Renderer->drawQuad(mark, CheckColor);
        }
    }

    void OnMouseEnter() override { m_Hovered = true; }
    void OnMouseLeave() override { m_Hovered = false; }

    FReply OnMouseButtonDown(const Vector2& /*pos*/, int button) override
    {
        if (button != 0)
            return FReply::Unhandled();
        m_Pressed = true;
        return FReply::Handled().CaptureMouse(this);
    }

    FReply OnMouseButtonUp(const Vector2& pos, int button) override
    {
        if (button != 0)
            return FReply::Unhandled();
        const bool was_pressed = m_Pressed;
        m_Pressed = false;
        if (was_pressed && m_CachedGeometry.IsUnderLocation(pos))
        {
            Checked = !Checked;
            if (OnCheckStateChanged)
                OnCheckStateChanged(Checked);
        }
        return FReply::Handled().ReleaseMouseCapture();
    }

private:
    bool m_Hovered {false};
    bool m_Pressed {false};
};
}  // namespace ZSlate
