#pragma once

#include "Runtime/Slate/Core/SlateGeometry.h"
#include "Runtime/Slate/Core/SlateKeys.h"
#include "Runtime/Slate/Core/SlatePaint.h"
#include "Runtime/Slate/Core/SlateReply.h"
#include "Runtime/UI/Render/UIRenderer.h"

#include <memory>
#include <vector>

namespace ZSlate
{
struct FDragDropOperation;

// Base class for every ZSlate widget (UE Slate SWidget analogue).
//
// Lifecycle per frame, driven by SlateApplication:
//   1. CacheDesiredSize()  -- bottom-up: each widget computes ComputeDesiredSize()
//                             after its children have cached theirs.
//   2. Paint()             -- top-down: the parent arranges children into
//                             geometries (ArrangeChildren), each child paints
//                             itself (OnPaint) then recurses.
//
// Widgets are always heap-allocated and shared via std::shared_ptr (Slate's
// TSharedRef model) so parents can hold children and event routing can keep weak
// references without lifetime surprises.
class SWidget : public std::enable_shared_from_this<SWidget>
{
public:
    virtual ~SWidget() = default;

    EVisibility Visibility {EVisibility::Visible};

    // --- Layout -------------------------------------------------------------
    Vector2 GetDesiredSize() const { return m_DesiredSize; }

    // Bottom-up desired-size cache. Recurses children first, then this widget.
    void CacheDesiredSize()
    {
        const int count = GetChildCount();
        for (int i = 0; i < count; ++i)
        {
            if (const std::shared_ptr<SWidget> child = GetChildAt(i))
                child->CacheDesiredSize();
        }
        m_DesiredSize = (Visibility == EVisibility::Collapsed) ? Vector2(0.0f, 0.0f) : ComputeDesiredSize();
    }

    // Pure intrinsic size of this widget (independent of allotted space).
    virtual Vector2 ComputeDesiredSize() const = 0;

    // --- Children -----------------------------------------------------------
    virtual int GetChildCount() const { return 0; }
    virtual std::shared_ptr<SWidget> GetChildAt(int /*index*/) const { return nullptr; }

    // Produce the geometry each visible child should occupy inside `allotted`.
    // Leaf widgets leave `out` empty.
    virtual void ArrangeChildren(const FGeometry& /*allotted*/, std::vector<FArrangedWidget>& /*out*/) const {}

    // --- Painting -----------------------------------------------------------
    // Draw THIS widget's own visuals (background, text, icon, ...). Children are
    // painted by the Paint() driver, not here.
    virtual void OnPaint(const FPaintContext& /*ctx*/, const FGeometry& /*geom*/) const {}

    // Non-virtual driver: paints self, then arranges + recurses into children.
    void Paint(const FPaintContext& ctx, const FGeometry& geom) const
    {
        if (Visibility == EVisibility::Collapsed || Visibility == EVisibility::Hidden)
            return;

        // Record where we were placed so the input router can hit-test next time.
        m_CachedGeometry = geom;

        OnPaint(ctx, geom);

        std::vector<FArrangedWidget> arranged;
        ArrangeChildren(geom, arranged);

        const bool clip = ClipsContent() && ctx.Renderer != nullptr;
        if (clip)
            ctx.Renderer->pushClipRect(geom.ToRect(), true);
        for (const FArrangedWidget& aw : arranged)
        {
            if (aw.Widget)
                aw.Widget->Paint(ctx, aw.Geometry);
        }
        if (clip)
            ctx.Renderer->popClipRect();
    }

    // Override to clip children to this widget's geometry (scroll views, etc.).
    virtual bool ClipsContent() const { return false; }

    // --- Input --------------------------------------------------------------
    // Only fully-Visible widgets participate in hit-testing.
    bool IsHitTestVisible() const { return Visibility == EVisibility::Visible; }

    // Last geometry this widget was painted into; used for hit-testing.
    const FGeometry& GetCachedGeometry() const { return m_CachedGeometry; }

    // Mouse events, routed by SlateInputRouter. Position is absolute screen px.
    virtual void OnMouseEnter() {}
    virtual void OnMouseLeave() {}
    virtual void OnMouseMove(const Vector2& /*screen_pos*/) {}
    virtual FReply OnMouseButtonDown(const Vector2& /*screen_pos*/, int /*button*/) { return FReply::Unhandled(); }
    virtual FReply OnMouseButtonUp(const Vector2& /*screen_pos*/, int /*button*/) { return FReply::Unhandled(); }
    virtual FReply OnMouseWheel(const Vector2& /*screen_pos*/, float /*delta*/) { return FReply::Unhandled(); }

    // Mouse capture was taken away without a matching button-up (e.g. a drag
    // gesture started, or the tree was rebuilt). Reset any "pressed" visuals.
    virtual void OnMouseCaptureLost() {}

    // Cursor to show while this widget is the deepest hovered widget (UE Slate
    // FCursorReply analogue). The host queries the hovered widget after routing
    // and applies the mapped platform cursor. Default keeps the arrow.
    virtual ECursorType GetCursor() const { return ECursorType::Default; }

    // --- Drag and drop ------------------------------------------------------
    // A press has moved past the drag threshold; return a non-null operation to
    // begin a drag with this widget as the source, or null to opt out.
    virtual std::shared_ptr<FDragDropOperation> OnDragDetected(const Vector2& /*screen_pos*/) { return nullptr; }
    // The dragged operation entered / left this widget's bounds.
    virtual void OnDragEnter(const std::shared_ptr<FDragDropOperation>& /*op*/) {}
    virtual void OnDragLeave(const std::shared_ptr<FDragDropOperation>& /*op*/) {}
    // The operation is hovering this widget. Return Handled to mark this widget a
    // valid drop target (drives enter/leave + receives OnDrop).
    virtual FReply OnDragOver(const Vector2& /*screen_pos*/, const std::shared_ptr<FDragDropOperation>& /*op*/) { return FReply::Unhandled(); }
    // The operation was released over this widget. Return Handled to consume it.
    virtual FReply OnDrop(const Vector2& /*screen_pos*/, const std::shared_ptr<FDragDropOperation>& /*op*/) { return FReply::Unhandled(); }

    // Keyboard focus + events (delivered to the focused widget only).
    virtual bool SupportsKeyboardFocus() const { return false; }
    virtual void OnFocusReceived() {}
    virtual void OnFocusLost() {}
    virtual void OnKeyChar(unsigned int /*codepoint*/) {}
    virtual FReply OnKeyDown(EKey /*key*/) { return FReply::Unhandled(); }

    // --- Per-frame update (optional) ---------------------------------------
    virtual void Tick(const FGeometry& /*allotted*/, float /*delta_seconds*/) {}

protected:
    mutable Vector2 m_DesiredSize {0.0f, 0.0f};
    mutable FGeometry m_CachedGeometry;
};
}  // namespace ZSlate
