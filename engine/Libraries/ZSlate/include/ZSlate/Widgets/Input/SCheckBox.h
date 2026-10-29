#pragma once

// SCheckBox: a checkable toggle box with a label.
// UE analogue: Widgets/Input/SCheckBox.h
// TODO: replace with full implementation from ZSlate submodule.

#include "ZSlate/Widgets/SCompoundWidget.h"

#include <functional>
#include <string>

namespace ZSlate
{

class SCheckBox : public SCompoundWidget
{
public:
    SCheckBox() = default;
    virtual ~SCheckBox() = default;

    // Public members accessed by UMG UCheckBox.h
    bool Checked {false};
    float BoxSize {18.0f};
    float FontSize {13.0f};
    Vector2 Size {200.0f, 24.0f};
    std::function<void(bool)> OnCheckStateChanged;
    std::string Label;

    void SetChecked(bool c) { Checked = c; }
    bool IsChecked() const { return Checked; }
    void SetLabel(const std::string& l) { Label = l; }

    Vector2 ComputeDesiredSize() const override { return Size; }
    void ArrangeChildren(const FGeometry& geom, std::vector<FArrangedWidget>& out) const override { (void)geom; (void)out; }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (!ctx.Renderer) return;

        const UIRect rect = geom.ToRect();

        // Hover highlight
        if (m_Hovered)
            ctx.Renderer->DrawQuad(rect, UIColor(0.16f, 0.17f, 0.21f, 1.0f));

        // Checkbox square
        const float sz = BoxSize;
        const UIRect box(rect.x + 4.0f, rect.y + (rect.h - sz) * 0.5f, sz, sz);
        UIColor boxColor = Checked ? UIColor(0.30f, 0.55f, 0.95f, 1.0f)
                                   : UIColor(0.20f, 0.20f, 0.22f, 1.0f);
        ctx.Renderer->DrawQuad(box, boxColor);
        ctx.Renderer->DrawRect(box, UIColor(0.35f, 0.38f, 0.45f, 1.0f), 1.0f);

        // Checkmark (simple ✓ cross)
        if (Checked)
        {
            float pad = 3.0f;
            UIRect mark(box.x + pad, box.y + pad, box.w - pad * 2, box.h - pad * 2);
            ctx.Renderer->DrawTextLabel(mark, "\xE2\x9C\x93", sz - 2.0f,
                                   UIColor(1.0f, 1.0f, 1.0f, 1.0f),
                                   TextAnchor::MiddleCenter, TextWrapMode::NoWrap);
        }

        // Label
        if (!Label.empty())
        {
            UIRect lr(rect.x + sz + 10.0f, rect.y, rect.w - sz - 14.0f, rect.h);
            ctx.Renderer->DrawTextLabel(lr, Label, FontSize, UIColor(0.88f, 0.89f, 0.92f, 1.0f),
                                   TextAnchor::MiddleLeft, TextWrapMode::NoWrap);
        }
    }

    // Input: click toggles check state
    void OnMouseEnter() override { m_Hovered = true; }
    void OnMouseLeave() override { m_Hovered = false; }

    FReply OnMouseButtonDown(const Vector2& pos, int button) override
    {
        if (button != 0) return FReply::Unhandled();
        (void)pos;
        Checked = !Checked;
        if (OnCheckStateChanged) OnCheckStateChanged(Checked);
        return FReply::Handled();
    }

private:
    mutable bool m_Hovered {false};
};

}  // namespace ZSlate
