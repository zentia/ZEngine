#include "ZSlate/Application/SlateApplication.h"

#include <algorithm>

namespace ZSlate
{
// Definition of the process-wide text measurer declared in SlatePaint.h.
ISlateTextMeasurer* GSlateTextMeasurer = nullptr;

// Definition of clipboard callbacks declared in SlatePaint.h.
SlateGetClipboardFunc GSlateGetClipboard = nullptr;
SlateSetClipboardFunc GSlateSetClipboard = nullptr;
void* GSlateClipboardUserData = nullptr;

// Global platform instance
ISlatePlatform* GPlatform = nullptr;

void SetPlatform(ISlatePlatform* platform)
{
    GPlatform = platform;
}

ISlatePlatform* GetPlatform()
{
    return GPlatform;
}

SlateApplication& SlateApplication::Get()
{
    static SlateApplication instance;
    return instance;
}

void SlateApplication::SetTextMeasurer(ISlateTextMeasurer* measurer)
{
    GSlateTextMeasurer = measurer;
}

void SlateApplication::SetClipboardCallbacks(SlateGetClipboardFunc getFunc,
                                             SlateSetClipboardFunc setFunc,
                                             void* userdata)
{
    GSlateGetClipboard = getFunc;
    GSlateSetClipboard = setFunc;
    GSlateClipboardUserData = userdata;
}

void SlateApplication::Tick(float delta_seconds)
{
    if (m_Root)
    {
        // Widgets that animate override Tick(); the root drives itself for now.
        m_Root->Tick(FGeometry(), delta_seconds);
    }
}

void SlateApplication::PaintInto(ISlateRenderer* renderer, const UIRect& region)
{
    m_ClosedModal.reset();

    if (renderer == nullptr || (m_Root == nullptr && m_Modal == nullptr))
        return;

    FPaintContext ctx;
    ctx.Renderer = renderer;
    ctx.LayerId = 0;

    renderer->PushClipRect(region);

    if (m_Root)
    {
        // 1. Bottom-up desired-size cache.
        m_Root->CacheDesiredSize();

        // 2. Top-down arrange + paint, clipped to the host region.
        const FGeometry root_geometry(Vector2(region.x, region.y), Vector2(region.w, region.h));
        m_Root->Paint(ctx, root_geometry);
    }

    if (m_Modal)
    {
        // Keep a reference: a widget inside the modal may close it mid-paint.
        const std::shared_ptr<SWidget> modal = m_Modal;

        renderer->DrawQuad(region, ModalScrimColor);

        modal->CacheDesiredSize();
        const Vector2 desired = modal->GetDesiredSize();
        const float w = std::min(desired.x, region.w);
        const float h = std::min(desired.y, region.h);
        const FGeometry modal_geometry(Vector2(region.x + (region.w - w) * 0.5f, region.y + (region.h - h) * 0.5f),
                                       Vector2(w, h));
        ctx.LayerId = 1;
        modal->Paint(ctx, modal_geometry);
    }

    // Popups (combo dropdowns, ...) go above the root and any modal.
    if (ISlatePopup* popup = SlateInputRouter::GetActivePopup())
        popup->PaintPopup(renderer, region);

    renderer->PopClipRect();
}
}  // namespace ZSlate
