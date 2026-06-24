#pragma once

#include "Runtime/UI/Render/UIAffine2D.h"
#include "Runtime/UI/Core/UITypes.h"

#include <cstdint>
#include <string>

class Font;

// ============================================================================
// UIRenderer
// ----------------------------------------------------------------------------
// Backend-agnostic 2D renderer for the ZEngine UGUI-like UI subsystem.
//
// Conceptually mirrors Unity UGUI's `CanvasRenderer` immediate-mode side: each
// `Graphic` (Image / Text / RawImage / ...) calls into `UIRenderer` during the
// `PreRender` pass to push quads / textured quads / glyph runs. `UIRenderer`
// is responsible for issuing them to whatever GPU pipeline the host platform
// has wired up.
//
// The live backend is `BatchedUIRenderer` (BatchedUIRenderer.cpp): it records
// primitives into a `UIRenderBatch` and submits them through the engine's own
// RHI pipeline, with text rasterised by the native `ZFontAtlas` (stb_truetype).
// Callers (`Image::onRender`, `Text::onRender`, ...) stay backend-agnostic.
// ============================================================================

class UIRenderer
{
public:
    UIRenderer() = default;
    virtual ~UIRenderer() = default;

    // Begin/End frame are called by `UISystem::PreRender()` once per frame,
    // around the retained-mode ZSlate/UMG paint. Some backends need them to
    // Flush. Returns false if no draw target is available this frame; in that
    // case the caller MUST skip draw* calls.
    virtual bool BeginFrame() { return true; }
    virtual void EndFrame() {}

    // Push a clip rect. `clip_rect` is in screen-space pixels (origin top-left).
    // Calls nest: `PushClipRect` must be paired with `PopClipRect`.
    virtual void PushClipRect(const UIRect& clip_rect, bool intersect_with_current = true) = 0;
    virtual void PopClipRect() = 0;

    // Cumulative 2D transform applied to subsequent draw calls (widget-local space).
    virtual void PushTransform(const UIAffine2D& transform) { (void)transform; }
    virtual void PopTransform() {}

    // Solid axis-aligned filled rectangle.
    virtual void DrawQuad(const UIRect& rect, const UIColor& color) = 0;

    // Outline (1-pixel default) rectangle.
    virtual void DrawRect(const UIRect& rect, const UIColor& color, float thickness = 1.0f) = 0;

    // Solid convex polygon (>= 3 points, in order), fan-triangulated. Used for
    // vector icons (play triangle, etc.) that a quad/outline can't express.
    // Default no-op: only BatchedUIRenderer (which paints editor chrome) needs
    // it; plain runtime UI never draws these icons.
    virtual void DrawConvexPoly(const Vector2* points, int count, const UIColor& color)
    {
        (void)points;
        (void)count;
        (void)color;
    }

    // Textured quad.
    //   texture_id: backend-specific opaque handle (cast to ImTextureID for the
    //               ImGui-backed implementation; nullptr-equivalent => fall
    //               through to a solid-colour quad).
    //   uv0/uv1   : top-left / bottom-right uv (default = full texture).
    virtual void DrawTexturedQuad(const UIRect& rect,
                                  void* texture_id,
                                  const UIColor& color = UIColor(1, 1, 1, 1),
                                  const Vector2& uv0 = Vector2(0.0f, 0.0f),
                                  const Vector2& uv1 = Vector2(1.0f, 1.0f)) = 0;

    // Text run; optional `font` resolves through UIGpuResources (TTF atlas).
    virtual void DrawText(const UIRect& rect,
                          const std::string& text,
                          float font_size,
                          const UIColor& color,
                          TextAnchor alignment = TextAnchor::MiddleCenter,
                          TextWrapMode wrap = TextWrapMode::Wrap,
                          Font* font = nullptr) = 0;

    virtual Vector2 MeasureText(const std::string& text,
                                float font_size,
                                TextWrapMode wrap = TextWrapMode::Wrap,
                                float wrap_width = 0.0f,
                                Font* font = nullptr) const = 0;

    // Current draw size in pixels (display framebuffer). UI uses this for
    // anchor/stretch resolution at the root Canvas.
    virtual Vector2 getDisplaySize() const = 0;
};

// Factory implementations live in BatchedUIRenderer.cpp / UIRenderer.cpp.
UIRenderer* CreateDefaultUIRenderer();
UIRenderer* CreateBatchedUIRenderer();
void DestroyUIRenderer(UIRenderer* renderer);
