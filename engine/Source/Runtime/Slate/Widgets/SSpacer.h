#pragma once

#include "Runtime/Slate/Widgets/SLeafWidget.h"

namespace ZSlate
{
// Empty layout filler with a fixed desired size (UE Slate SSpacer analogue).
class SSpacer : public SLeafWidget
{
public:
    Vector2 Size {0.0f, 0.0f};

    explicit SSpacer(const Vector2& size = Vector2(0.0f, 0.0f))
        : Size(size) {}

    Vector2 ComputeDesiredSize() const override { return Size; }
};
}  // namespace ZSlate
