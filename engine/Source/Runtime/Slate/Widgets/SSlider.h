#pragma once

#include "Runtime/Slate/Widgets/SLeafWidget.h"

#include <functional>

namespace ZSlate
{
// A horizontal value slider in [0,1] (UE Slate SSlider analogue). Click/drag the
// track to set the value; reports via OnValueChanged. Width stretches in a Fill
// slot; height is fixed.
class SSlider : public SLeafWidget
{
public:
    float Value {0.0f};
    float MinDesiredWidth {120.0f};
    float Height {18.0f};
    float HandleWidth {10.0f};

    UIColor TrackColor {0.18f, 0.18f, 0.21f, 1.0f};
    UIColor FillColor {0.30f, 0.50f, 0.85f, 1.0f};
    UIColor HandleColor {0.85f, 0.87f, 0.92f, 1.0f};
    UIColor HandleHoverColor {1.0f, 1.0f, 1.0f, 1.0f};

    std::function<void(float)> OnValueChanged;

    Vector2 ComputeDesiredSize() const override { return Vector2(MinDesiredWidth, Height); }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (ctx.Renderer == nullptr)
            return;
        const UIRect rect = geom.ToRect();

        const float track_h = 4.0f;
        const float track_y = rect.y + (rect.height - track_h) * 0.5f;
        const UIRect track(rect.x, track_y, rect.width, track_h);
        ctx.Renderer->drawQuad(track, TrackColor);

        const float usable = rect.width - HandleWidth;
        const float fill_w = HandleWidth * 0.5f + usable * Clamp01(Value);
        ctx.Renderer->drawQuad(UIRect(rect.x, track_y, fill_w, track_h), FillColor);

        const float handle_x = rect.x + usable * Clamp01(Value);
        const UIRect handle(handle_x, rect.y, HandleWidth, rect.height);
        ctx.Renderer->drawQuad(handle, (m_Hovered || m_Dragging) ? HandleHoverColor : HandleColor);
    }

    void OnMouseEnter() override { m_Hovered = true; }
    void OnMouseLeave() override { m_Hovered = false; }

    FReply OnMouseButtonDown(const Vector2& pos, int button) override
    {
        if (button != 0)
            return FReply::Unhandled();
        m_Dragging = true;
        SetValueFromPosition(pos);
        return FReply::Handled().CaptureMouse(this);
    }

    void OnMouseMove(const Vector2& pos) override
    {
        if (m_Dragging)
            SetValueFromPosition(pos);
    }

    FReply OnMouseButtonUp(const Vector2& /*pos*/, int button) override
    {
        if (button != 0)
            return FReply::Unhandled();
        m_Dragging = false;
        return FReply::Handled().ReleaseMouseCapture();
    }

private:
    static float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

    void SetValueFromPosition(const Vector2& pos)
    {
        const UIRect rect = m_CachedGeometry.ToRect();
        const float usable = rect.width - HandleWidth;
        float t = (usable > 0.0f) ? (pos.x - rect.x - HandleWidth * 0.5f) / usable : 0.0f;
        t = Clamp01(t);
        if (t != Value)
        {
            Value = t;
            if (OnValueChanged)
                OnValueChanged(Value);
        }
    }

    bool m_Hovered {false};
    bool m_Dragging {false};
};
}  // namespace ZSlate
