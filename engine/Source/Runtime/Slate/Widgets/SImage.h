#pragma once

#include "Runtime/Slate/Widgets/SLeafWidget.h"

namespace ZSlate
{
// A textured (or solid-tinted) quad. Texture is a backend-opaque handle; null
// renders a solid Tint quad (UIRenderer fallback).
class SImage : public SLeafWidget
{
public:
    void* Texture {nullptr};
    UIColor Tint {1.0f, 1.0f, 1.0f, 1.0f};
    Vector2 DesiredSize {16.0f, 16.0f};
    Vector2 Uv0 {0.0f, 0.0f};
    Vector2 Uv1 {1.0f, 1.0f};

    Vector2 ComputeDesiredSize() const override { return DesiredSize; }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (ctx.Renderer == nullptr)
            return;
        ctx.Renderer->drawTexturedQuad(geom.ToRect(), Texture, Tint, Uv0, Uv1);
    }
};
}  // namespace ZSlate
