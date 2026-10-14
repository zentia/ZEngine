#pragma once

// ============================================================================
// ZSlate/Core/SlatePaint.h
//
// Paint primitives: the renderer interface widgets draw through, geometry,
// arranged children and input replies.
// ============================================================================

#include "ZSlateTypes.h"

#include <memory>
#include <vector>

namespace ZSlate
{

class SWidget;

// ---------------------------------------------------------------------------
// ISlateRenderer — the only drawing API a widget ever sees.
// ---------------------------------------------------------------------------
class ISlateRenderer
{
public:
    virtual ~ISlateRenderer() = default;

    // Filled rectangle.
    virtual void DrawQuad(const UIRect& rect, const UIColor& color) = 0;

    // Outlined rectangle.
    virtual void DrawRect(const UIRect& rect, const UIColor& color, float thickness = 1.0f) = 0;

    // Text. `fontInfo` is reserved (pass nullptr).
    virtual void DrawTextLabel(const UIRect& rect,
                               const std::string& text,
                               float fontSize,
                               const UIColor& color,
                               TextAnchor anchor = TextAnchor::MiddleLeft,
                               TextWrapMode wrap = TextWrapMode::NoWrap,
                               void* fontInfo = nullptr) = 0;

    // Optional scissoring. Renderers that do not implement it simply ignore
    // these calls, so widgets may use them unconditionally.
    virtual void PushClipRect(const UIRect& rect) { (void)rect; }
    virtual void PopClipRect() {}
};

// ---------------------------------------------------------------------------
// FGeometry — absolute position + size of a widget for one frame.
// ---------------------------------------------------------------------------
struct FGeometry
{
    Vector2 Offset   { 0.0f, 0.0f };
    Vector2 LocalSize{ 0.0f, 0.0f };

    FGeometry() = default;
    FGeometry(const Vector2& inOffset, const Vector2& inSize)
        : Offset(inOffset), LocalSize(inSize) {}

    UIRect ToRect() const { return UIRect(Offset.x, Offset.y, LocalSize.x, LocalSize.y); }

    // Child geometry from a local offset/size.
    FGeometry MakeChild(const Vector2& localOffset, const Vector2& size) const
    {
        return FGeometry(Offset + localOffset, size);
    }

    // Absolute position -> local position.
    Vector2 ToLocal(const Vector2& absolute) const { return absolute - Offset; }
};

// ---------------------------------------------------------------------------
// FArrangedChild — a child together with the geometry it was given.
// ---------------------------------------------------------------------------
struct FArrangedChild
{
    std::shared_ptr<SWidget> Widget;
    FGeometry                Geometry;

    FArrangedChild() = default;
    FArrangedChild(const std::shared_ptr<SWidget>& inWidget, const FGeometry& inGeometry)
        : Widget(inWidget), Geometry(inGeometry) {}
};

// ---------------------------------------------------------------------------
// FReply — did a widget consume an input event?
// ---------------------------------------------------------------------------
struct FReply
{
    bool HandledFlag = false;

    static FReply Handled()   { FReply r; r.HandledFlag = true;  return r; }
    static FReply Unhandled() { FReply r; r.HandledFlag = false; return r; }

    bool IsHandled()   const { return HandledFlag; }
    bool IsUnhandled() const { return !HandledFlag; }
};

// ---------------------------------------------------------------------------
// FPaintContext — handed to SWidget::OnPaint.
// ---------------------------------------------------------------------------
struct FPaintContext
{
    ISlateRenderer* Renderer = nullptr;
};

} // namespace ZSlate
