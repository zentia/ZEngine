#pragma once

#include "ZSlate/Widgets/SLeafWidget.h"

namespace ZSlate
{
// A slider widget (minimal implementation).
// Similar to UE's SSlider.
class SSlider : public SLeafWidget
{
public:
    float Value {0.0f};
    float MinValue {0.0f};
    float MaxValue {1.0f};
    bool bIsHorizontal {true};

    std::function<void(float)> OnValueChanged;

    Vector2 ComputeDesiredSize() const override
    {
        if (bIsHorizontal)
            return Vector2(100.0f, 20.0f);
        else
            return Vector2(20.0f, 100.0f);
    }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (ctx.Renderer == nullptr)
            return;

        // Draw background
        UIRect rect = geom.ToRect();
        ctx.Renderer->DrawRect(rect, UIColor(0.2f, 0.2f, 0.2f, 1.0f), 1.0f);

        // Draw thumb
        float thumb_size = 10.0f;
        float thumb_pos;
        if (bIsHorizontal)
        {
            thumb_pos = rect.x + (rect.w - thumb_size) * ((Value - MinValue) / (MaxValue - MinValue + 0.0001f));
            ctx.Renderer->DrawQuad(UIRect(thumb_pos, rect.y + (rect.h - thumb_size) * 0.5f, thumb_size, thumb_size),
                                   UIColor(0.5f, 0.5f, 0.5f, 1.0f));
        }
        else
        {
            thumb_pos = rect.y + (rect.h - thumb_size) * ((Value - MinValue) / (MaxValue - MinValue + 0.0001f));
            ctx.Renderer->DrawQuad(UIRect(rect.x + (rect.w - thumb_size) * 0.5f, thumb_pos, thumb_size, thumb_size),
                                   UIColor(0.5f, 0.5f, 0.5f, 1.0f));
        }
    }

    FReply OnMouseButtonDown(const Vector2& pos, int button) override
    {
        if (button != 0)
            return FReply::Unhandled();
        m_Dragging = true;
        UpdateValueFromMouse(pos);
        return FReply::Handled().CaptureMouse(const_cast<SSlider*>(this));
    }

    void OnMouseMove(const Vector2& pos) override
    {
        if (!m_Dragging) return;
        UpdateValueFromMouse(pos);
    }

    FReply OnMouseButtonUp(const Vector2& /*pos*/, int button) override
    {
        if (button != 0 || !m_Dragging)
            return FReply::Unhandled();
        m_Dragging = false;
        return FReply::Handled().ReleaseMouseCapture();
    }

    void OnMouseCaptureLost() override { m_Dragging = false; }

private:
    mutable bool m_Dragging {false};

    void UpdateValueFromMouse(const Vector2& pos)
    {
        FGeometry geom = GetCachedGeometry();
        UIRect rect = geom.ToRect();
        float new_value;
        if (bIsHorizontal)
        {
            float t = (pos.x - rect.x) / (rect.w + 0.0001f);
            new_value = MinValue + t * (MaxValue - MinValue);
        }
        else
        {
            float t = (pos.y - rect.y) / (rect.h + 0.0001f);
            new_value = MinValue + t * (MaxValue - MinValue);
        }

        new_value = std::max(MinValue, std::min(MaxValue, new_value));
        if (new_value != Value)
        {
            Value = new_value;
            if (OnValueChanged)
                OnValueChanged(Value);
        }
    }
};
}  // namespace ZSlate
