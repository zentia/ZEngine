#pragma once

#include "ZSlate/Widgets/SCompoundWidget.h"

namespace ZSlate
{
// A box that sizes its single child according to size constraints.
// Similar to UE's SBox.
class SBox : public SCompoundWidget
{
public:
    float WidthOverride {-1.0f};
    float HeightOverride {-1.0f};
    float MinDesiredWidth {-1.0f};
    float MinDesiredHeight {-1.0f};
    float MaxDesiredWidth {-1.0f};
    float MaxDesiredHeight {-1.0f};
    EHorizontalAlignment HAlign {EHorizontalAlignment::Center};
    EVerticalAlignment VAlign {EVerticalAlignment::Center};

    Vector2 ComputeDesiredSize() const override
    {
        Vector2 child_size(0.0f, 0.0f);
        if (m_Child)
            child_size = m_Child->ComputeDesiredSize();

        float w = child_size.x;
        float h = child_size.y;

        if (WidthOverride >= 0.0f)
            w = WidthOverride;
        if (HeightOverride >= 0.0f)
            h = HeightOverride;

        if (MinDesiredWidth >= 0.0f && w < MinDesiredWidth)
            w = MinDesiredWidth;
        if (MinDesiredHeight >= 0.0f && h < MinDesiredHeight)
            h = MinDesiredHeight;

        if (MaxDesiredWidth >= 0.0f && w > MaxDesiredWidth)
            w = MaxDesiredWidth;
        if (MaxDesiredHeight >= 0.0f && h > MaxDesiredHeight)
            h = MaxDesiredHeight;

        return Vector2(w, h);
    }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (m_Child && ctx.Renderer)
        {
            m_Child->Paint(ctx, geom);
        }
    }
};
}  // namespace ZSlate
