#pragma once

#include "ZSlate/Core/SlateGeometry.h"
#include "ZSlate/Core/ZSlateTypes.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ZSlate
{
// ============================================================================
// ISlateRenderer - Abstract rendering backend
// ============================================================================
// The host (engine or standalone app) provides an implementation of this
// interface. ZSlate never touches the concrete rendering API directly.

struct ISlateRenderer
{
    virtual ~ISlateRenderer() = default;

    // Drawing primitives
    virtual void DrawQuad(const UIRect& rect, const UIColor& color) = 0;
    virtual void DrawRect(const UIRect& rect, const UIColor& color, float thickness = 1.0f) = 0;
    virtual void DrawConvexPoly(const Vector2* points, int count, const UIColor& color) = 0;
    virtual void DrawRoundedRect(const UIRect& rect, float radius, const UIColor& color) = 0;
    virtual void DrawTexturedQuad(const UIRect& rect, void* texture_handle, const UIColor& tint = Colors::White) = 0;
    virtual void DrawBox(const UIRect& rect, const FMargin& margin, void* texture_handle, const UIColor& tint) = 0;
    virtual void DrawBorder(const UIRect& rect, const FMargin& margin, void* texture_handle, const UIColor& tint) = 0;

    // Text
    // Named DrawTextLabel to avoid collision with windows.h DrawText macro.
    // Overload 1: draw into a rect with alignment (used by most widgets)
    virtual void DrawTextLabel(const UIRect& rect, const std::string& text, float font_size, const UIColor& color,
                               TextAnchor alignment = TextAnchor::MiddleLeft, TextWrapMode wrap = TextWrapMode::NoWrap,
                               void* font_handle = nullptr) = 0;
    // Overload 2: draw at position (legacy API)
    virtual void DrawTextLabel(const std::string& text, const Vector2& pos, float font_size, const UIColor& color) = 0;
    virtual Vector2 MeasureText(const std::string& text, float font_size) const = 0;

    // Clipping
    virtual void PushClipRect(const UIRect& rect) = 0;
    virtual void PopClipRect() = 0;

    // State
    virtual void BeginFrame() = 0;
    virtual void EndFrame() = 0;
    virtual void Flush() = 0;
};

// ============================================================================
// FPaintContext - Everything a widget needs to emit draw calls
// ============================================================================

struct FPaintContext
{
    ISlateRenderer* Renderer {nullptr};
    // Monotonic paint depth; later used for clip-stack debugging / z-ordering.
    int LayerId {0};
};

// ============================================================================
// ISlateTextMeasurer - Layout-time text measurement
// ============================================================================
// ComputeDesiredSize() runs before painting, so it cannot reach a live
// UIRenderer frame; instead SlateApplication installs a measurer that
// forwards to ISlateRenderer::MeasureText (or the editor font atlas).
class ISlateTextMeasurer
{
public:
    virtual ~ISlateTextMeasurer() = default;
    virtual Vector2 Measure(const std::string& text, float font_size) const = 0;
};

// Process-wide measurer installed by SlateApplication::SetTextMeasurer (may be null).
extern ISlateTextMeasurer* GSlateTextMeasurer;

// ============================================================================
// ISlatePlatform - All platform abstractions in one place
// ============================================================================
struct ISlatePlatform
{
    virtual ~ISlatePlatform() = default;

    // Renderer
    virtual ISlateRenderer* GetRenderer() = 0;

    // Input (host pumps these each frame)
    virtual Vector2 GetMousePosition() const = 0;
    virtual bool IsMouseButtonDown(int button) const = 0;
    virtual bool IsKeyDown(int key) const = 0;
    virtual float GetTimeSeconds() const = 0;

    // Window (for DPI, etc.)
    virtual Vector2 GetWindowSize() const = 0;
    virtual float GetDpiScale() const { return 1.0f; }
};

// ============================================================================
// Global Platform Instance
// ============================================================================

void SetPlatform(ISlatePlatform* platform);
ISlatePlatform* GetPlatform();
}  // namespace ZSlate
