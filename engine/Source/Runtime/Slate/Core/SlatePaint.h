#pragma once

#include "Runtime/Slate/Core/SlateGeometry.h"
#include "Runtime/Core/Math/Vector2.h"

class UIRenderer;

namespace ZSlate
{
// Everything a widget needs to emit draw calls for one frame. The renderer is
// ZUGUI's BatchedUIRenderer (UIRenderer) so ZSlate inherits a multi-backend,
// RHI-abstracted 2D pipeline + font atlas for free.
struct FPaintContext
{
    UIRenderer* Renderer {nullptr};
    // Monotonic paint depth; later used for clip-stack debugging / z-ordering.
    int LayerId {0};
};

// Layout-time text measurement. ComputeDesiredSize() runs before painting, so it
// cannot reach a live UIRenderer frame; instead SlateApplication installs a
// measurer that forwards to UIRenderer::measureText (or the editor font atlas).
class ISlateTextMeasurer
{
public:
    virtual ~ISlateTextMeasurer() = default;
    virtual Vector2 Measure(const std::string& text, float font_size) const = 0;
};

// Process-wide measurer, installed by SlateApplication::SetTextMeasurer.
// Null until installed; STextBlock falls back to a coarse estimate.
extern ISlateTextMeasurer* GSlateTextMeasurer;
}  // namespace ZSlate
