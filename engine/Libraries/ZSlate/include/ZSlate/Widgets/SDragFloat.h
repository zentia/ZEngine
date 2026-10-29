#pragma once

#include "ZSlate/Widgets/SLeafWidget.h"

#include <cstdio>
#include <functional>
#include <string>

namespace ZSlate
{
// A click-drag numeric field (UE Slate SSpinBox / ImGui DragFloat analogue).
// Drag horizontally to change the value by Speed per pixel; the value is
// clamped to [MinValue, MaxValue] when MaxValue > MinValue (otherwise the
// range is treated as unbounded). Reports edits via OnValueChanged.
//
// Editing is drag-only in this first iteration (no inline text entry); pair it
// with a sibling SEditableTextBox if precise keyboard entry is needed.
class SDragFloat : public SLeafWidget
{
public:
    float Value {0.0f};
    float MinValue {0.0f};
    float MaxValue {0.0f};  // MaxValue <= MinValue => unbounded
    float Speed {0.01f};

    float MinDesiredWidth {80.0f};
    float Height {22.0f};
    float FontSize {14.0f};
    std::string Format {"%.3f"};

    UIColor BackgroundColor {0.12f, 0.12f, 0.15f, 1.0f};
    UIColor HoverColor {0.16f, 0.16f, 0.20f, 1.0f};
    UIColor TextColor {0.85f, 0.87f, 0.92f, 1.0f};

    std::function<void(float)> OnValueChanged;

    Vector2 ComputeDesiredSize() const override { return Vector2(MinDesiredWidth, Height); }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (ctx.Renderer == nullptr)
            return;
        const UIRect rect = geom.ToRect();
        ctx.Renderer->DrawQuad(rect, (m_Hovered || m_Dragging) ? HoverColor : BackgroundColor);

        char buf[48];
        std::snprintf(buf, sizeof(buf), Format.c_str(), Value);
        ctx.Renderer->DrawTextLabel(rect, buf, FontSize, TextColor, TextAnchor::MiddleCenter, TextWrapMode::NoWrap);
    }

    void OnMouseEnter() override { m_Hovered = true; }
    void OnMouseLeave() override { m_Hovered = false; }

    FReply OnMouseButtonDown(const Vector2& pos, int button) override
    {
        if (button != 0)
            return FReply::Unhandled();
        m_Dragging = true;
        m_LastX = pos.x;
        return FReply::Handled().CaptureMouse(this);
    }

    void OnMouseMove(const Vector2& pos) override
    {
        if (!m_Dragging)
            return;
        const float dx = pos.x - m_LastX;
        m_LastX = pos.x;
        if (dx == 0.0f)
            return;
        float next = Value + dx * Speed;
        if (MaxValue > MinValue)
            next = next < MinValue ? MinValue : (next > MaxValue ? MaxValue : next);
        if (next != Value)
        {
            Value = next;
            if (OnValueChanged)
                OnValueChanged(Value);
        }
    }

    FReply OnMouseButtonUp(const Vector2& /*pos*/, int button) override
    {
        if (button != 0)
            return FReply::Unhandled();
        m_Dragging = false;
        return FReply::Handled().ReleaseMouseCapture();
    }

    void OnMouseCaptureLost() override { m_Dragging = false; }

private:
    bool m_Hovered {false};
    bool m_Dragging {false};
    float m_LastX {0.0f};
};
}  // namespace ZSlate
