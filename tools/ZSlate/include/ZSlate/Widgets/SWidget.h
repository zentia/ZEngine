#pragma once

// ============================================================================
// ZSlate/Widgets/SWidget.h
//
// Base class of every ZSlate widget.
//
// Contract
//  * One pass per frame: SlateApplication::PaintInto() walks the tree, gives
//    each widget its absolute FGeometry (cached on the widget so input can
//    hit-test against it) and asks it to paint itself, then arranges + paints
//    its children.
//  * Input is delivered in absolute (window) coordinates.
//  * Configuration lives in public fields; no invalidation is required
//    because every frame repaints the whole tree.
// ============================================================================

#include "../Core/SlatePaint.h"
#include "../Core/ZSlateTypes.h"

#include <memory>
#include <vector>
#include <string>

namespace ZSlate
{

class SWidget : public std::enable_shared_from_this<SWidget>
{
public:
    virtual ~SWidget() = default;

    // ---- Layout ------------------------------------------------------------
    // Desired (minimum) size. Returning 0 on an axis means "no opinion".
    virtual Vector2 ComputeDesiredSize() const { return Vector2(0.0f, 0.0f); }

    // Ask for the leftover space of the parent along the main axis.
    virtual bool WantsFillWidth()  const { return false; }
    virtual bool WantsFillHeight() const { return false; }

    // Fill in children (absolute geometry). Default: no children.
    virtual void OnArrangeChildren(const FGeometry& allottedGeometry,
                                   std::vector<FArrangedChild>& outChildren) const {}

    // ---- Paint -------------------------------------------------------------
    virtual void OnPaint(const FPaintContext& context, const FGeometry& geometry) const {}

    // ---- Input (absolute / window coordinates) -----------------------------
    virtual FReply OnMouseButtonDown(const Vector2& absolutePos, int button) { return FReply::Unhandled(); }
    virtual FReply OnMouseButtonUp  (const Vector2& absolutePos, int button) { return FReply::Unhandled(); }
    virtual FReply OnMouseMove      (const Vector2& absolutePos)             { return FReply::Unhandled(); }

    // Fired on the second press of a double-click, *after* OnMouseButtonDown.
    virtual FReply OnMouseButtonDoubleClick(const Vector2& absolutePos, int button)
    {
        return FReply::Unhandled();
    }

    virtual FReply OnMouseWheel     (const Vector2& absolutePos, float delta){ return FReply::Unhandled(); }
    virtual FReply OnKeyDown        (uint32_t keyCode, bool repeat)          { return FReply::Unhandled(); }
    virtual FReply OnKeyChar        (uint32_t codepoint)                     { return FReply::Unhandled(); }

    // ---- Focus -------------------------------------------------------------
    virtual bool IsFocusable() const { return false; }
    virtual void OnFocusGained() {}
    virtual void OnFocusLost()   {}

    // ---- State -------------------------------------------------------------
    const FGeometry& GetCachedGeometry() const { return m_CachedGeometry; }
    void             SetCachedGeometry(const FGeometry& geometry) { m_CachedGeometry = geometry; }

    bool IsVisible() const { return m_Visible; }
    void SetVisible(bool visible) { m_Visible = visible; }

    bool IsEnabled() const { return m_Enabled; }
    void SetEnabled(bool enabled) { m_Enabled = enabled; }

    bool IsHovered() const { return m_Hovered; }
    void SetHovered(bool hovered) { m_Hovered = hovered; }

    bool HasKeyboardFocus() const { return m_HasFocus; }
    void SetKeyboardFocus(bool focused) { m_HasFocus = focused; }

    SWidget* GetParent() const { return m_Parent; }
    void     SetParent(SWidget* parent) { m_Parent = parent; }

    // Optional identifier, handy for debugging and for host code that needs to
    // find a widget after a rebuild.
    const std::string& GetTag() const { return m_Tag; }
    void SetTag(std::string tag) { m_Tag = std::move(tag); }

private:
    FGeometry   m_CachedGeometry;
    SWidget*    m_Parent   = nullptr;
    std::string m_Tag;
    bool        m_Visible  = true;
    bool        m_Enabled  = true;
    bool        m_Hovered  = false;
    bool        m_HasFocus = false;
};

// Recursive paint helper: sets geometry, paints, then arranges + paints
// children. Shared by SlateApplication and by composite widgets (list rows…).
void PaintWidget(const std::shared_ptr<SWidget>& widget,
                 const FPaintContext& context,
                 const FGeometry& geometry);

} // namespace ZSlate
