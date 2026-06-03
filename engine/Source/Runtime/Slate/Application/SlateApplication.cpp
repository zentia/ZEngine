#include "Runtime/Slate/Application/SlateApplication.h"

#include "Runtime/UI/Render/UIRenderer.h"

namespace ZSlate
{
// Definition of the process-wide text measurer declared in SlatePaint.h.
ISlateTextMeasurer* GSlateTextMeasurer = nullptr;

SlateApplication& SlateApplication::Get()
{
    static SlateApplication instance;
    return instance;
}

void SlateApplication::SetTextMeasurer(ISlateTextMeasurer* measurer)
{
    GSlateTextMeasurer = measurer;
}

void SlateApplication::Tick(float delta_seconds)
{
    if (m_Root)
    {
        // Widgets that animate override Tick(); the root drives itself for now.
        m_Root->Tick(FGeometry(), delta_seconds);
    }
}

void SlateApplication::PaintInto(UIRenderer* renderer, const UIRect& region)
{
    if (m_Root == nullptr || renderer == nullptr)
        return;

    // 1. Bottom-up desired-size cache.
    m_Root->CacheDesiredSize();

    // 2. Top-down arrange + paint, clipped to the host region.
    const FGeometry root_geometry(Vector2(region.x, region.y), Vector2(region.width, region.height));
    FPaintContext ctx;
    ctx.Renderer = renderer;
    ctx.LayerId = 0;

    renderer->pushClipRect(region, /*intersect_with_current=*/true);
    m_Root->Paint(ctx, root_geometry);
    renderer->popClipRect();
}
}  // namespace ZSlate
