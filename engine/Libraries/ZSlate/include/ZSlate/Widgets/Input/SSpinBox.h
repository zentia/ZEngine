#pragma once

#include "ZSlate/Widgets/SCompoundWidget.h"

#include <functional>
#include <string>
#include <cmath>

namespace ZSlate
{

// =============================================================================
// SSpinBox — numeric spinner (UE SSpinBox analogue)
// =============================================================================
//
// Editable number with +/- buttons and drag-to-edit.
//
class SSpinBox : public SCompoundWidget
{
public:
    float Value {0.0f};
    float MinValue {0.0f};
    float MaxValue {100.0f};
    float Step {1.0f};
    int32_t DecimalPlaces {1};

    float FontSize {13.0f};

    std::function<void(float)> OnValueChanged;

    Vector2 ComputeDesiredSize() const override { return Vector2(120.0f, 26.0f); }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (!ctx.Renderer) return;
        const UIRect r = geom.ToRect();

        // Background
        UIColor bg = m_Dragging ? UIColor(0.22f, 0.24f, 0.30f, 1.0f)
                   : m_Hovered  ? UIColor(0.16f, 0.17f, 0.21f, 1.0f)
                   :              UIColor(0.12f, 0.12f, 0.14f, 1.0f);
        ctx.Renderer->DrawQuad(r, bg);
        ctx.Renderer->DrawRect(r, UIColor(0.35f, 0.38f, 0.45f, 1.0f), 1.0f);

        // Minus button
        float btnW = 22.0f;
        ctx.Renderer->DrawRect(UIRect(r.x + btnW, r.y, 1.0f, r.h),
                               UIColor(0.25f, 0.28f, 0.35f, 1.0f), 1.0f);
        ctx.Renderer->DrawTextLabel(
            UIRect(r.x, r.y, btnW, r.h), "-", FontSize + 2.0f,
            UIColor(0.7f, 0.7f, 0.7f, 1.0f),
            TextAnchor::MiddleCenter, TextWrapMode::NoWrap);

        // Plus button
        ctx.Renderer->DrawRect(UIRect(r.Right() - btnW - 1.0f, r.y, 1.0f, r.h),
                               UIColor(0.25f, 0.28f, 0.35f, 1.0f), 1.0f);
        ctx.Renderer->DrawTextLabel(
            UIRect(r.Right() - btnW, r.y, btnW, r.h), "+", FontSize + 2.0f,
            UIColor(0.7f, 0.7f, 0.7f, 1.0f),
            TextAnchor::MiddleCenter, TextWrapMode::NoWrap);

        // Value display
        char buf[32];
        if (DecimalPlaces == 0)
            std::snprintf(buf, sizeof(buf), "%.0f", Value);
        else
            std::snprintf(buf, sizeof(buf), "%.*f", DecimalPlaces, Value);

        ctx.Renderer->DrawTextLabel(
            UIRect(r.x + btnW + 2.0f, r.y, r.w - 2.0f * btnW - 4.0f, r.h),
            buf, FontSize,
            m_Dragging ? UIColor(0.5f, 0.8f, 1.0f, 1.0f) : UIColor(0.88f, 0.89f, 0.92f, 1.0f),
            TextAnchor::MiddleCenter, TextWrapMode::NoWrap);
    }

    void OnMouseEnter() override { m_Hovered = true; }
    void OnMouseLeave() override { m_Hovered = false; }

    FReply OnMouseButtonDown(const Vector2& pos, int btn) override
    {
        if (btn != 0) return FReply::Unhandled();
        const UIRect r = GetCachedGeometry().ToRect();
        float btnW = 22.0f;

        if (pos.x < r.x + btnW)
        {
            // Minus button
            SetValue(Value - Step);
            return FReply::Handled();
        }
        if (pos.x > r.Right() - btnW)
        {
            // Plus button
            SetValue(Value + Step);
            return FReply::Handled();
        }

        // Center area — begin drag
        m_Dragging = true;
        m_DragStartValue = Value;
        m_DragStartY = pos.y;
        return FReply::Handled().CaptureMouse(const_cast<SSpinBox*>(this));
    }

    void OnMouseMove(const Vector2& pos) override
    {
        if (!m_Dragging) return;
        float delta = m_DragStartY - pos.y;  // up = increase
        float sensitivity = (MaxValue - MinValue) / 200.0f;
        SetValue(m_DragStartValue + delta * sensitivity);
    }

    FReply OnMouseButtonUp(const Vector2&, int btn) override
    {
        if (btn != 0 || !m_Dragging) return FReply::Unhandled();
        m_Dragging = false;
        return FReply::Handled().ReleaseMouseCapture();
    }

    void OnMouseCaptureLost() override { m_Dragging = false; }

    FReply OnMouseWheel(const Vector2&, float delta) override
    {
        SetValue(Value + delta * Step * 0.5f);
        return FReply::Handled();
    }

private:
    mutable bool m_Hovered {false};
    mutable bool m_Dragging {false};
    mutable float m_DragStartValue {0.0f};
    mutable float m_DragStartY {0.0f};

    void SetValue(float v)
    {
        v = std::clamp(v, MinValue, MaxValue);
        if (std::abs(v - Value) < 0.0001f) return;
        Value = v;
        if (OnValueChanged) OnValueChanged(Value);
    }
};

}  // namespace ZSlate
