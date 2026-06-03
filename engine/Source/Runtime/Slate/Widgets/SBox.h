#pragma once

#include "Runtime/Slate/Widgets/SCompoundWidget.h"

namespace ZSlate
{
// A layout-only wrapper that can pin / clamp the size of its single child
// (UE Slate SBox analogue). A negative override means "use the child's desired
// size". Min/Max clamp the resulting desired size.
class SBox : public SCompoundWidget
{
public:
    float WidthOverride {-1.0f};
    float HeightOverride {-1.0f};
    float MinDesiredWidth {-1.0f};
    float MinDesiredHeight {-1.0f};
    float MaxDesiredWidth {-1.0f};
    float MaxDesiredHeight {-1.0f};

    Vector2 ComputeDesiredSize() const override
    {
        Vector2 size = SCompoundWidget::ComputeDesiredSize();

        if (WidthOverride >= 0.0f)
            size.x = WidthOverride;
        if (HeightOverride >= 0.0f)
            size.y = HeightOverride;

        if (MinDesiredWidth >= 0.0f && size.x < MinDesiredWidth)
            size.x = MinDesiredWidth;
        if (MinDesiredHeight >= 0.0f && size.y < MinDesiredHeight)
            size.y = MinDesiredHeight;
        if (MaxDesiredWidth >= 0.0f && size.x > MaxDesiredWidth)
            size.x = MaxDesiredWidth;
        if (MaxDesiredHeight >= 0.0f && size.y > MaxDesiredHeight)
            size.y = MaxDesiredHeight;

        return size;
    }
};
}  // namespace ZSlate
