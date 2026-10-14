#include "ZSlate/Application/SlateInput.h"
#include "ZSlate/Application/SlateApplication.h"

#include <chrono>
#include <cmath>

namespace ZSlate
{

// ---------------------------------------------------------------------------
// Hit testing
// ---------------------------------------------------------------------------
namespace
{
    double NowMilliseconds()
    {
        using clock = std::chrono::steady_clock;
        return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
    }

    // Walks the tree using the geometry produced by the last paint pass.
    // The deepest widget containing `pos` wins.
    void HitTestRecursive(const std::shared_ptr<SWidget>& widget,
                          const FGeometry& geometry,
                          const Vector2& absolutePos,
                          std::shared_ptr<SWidget>& outBest)
    {
        if (!widget || !widget->IsVisible() || !widget->IsEnabled()) return;
        if (!geometry.ToRect().Contains(absolutePos)) return;

        outBest = widget;

        std::vector<FArrangedChild> children;
        widget->OnArrangeChildren(geometry, children);
        for (const FArrangedChild& child : children)
        {
            HitTestRecursive(child.Widget, child.Geometry, absolutePos, outBest);
        }
    }
}

// ---------------------------------------------------------------------------
// Focus
// ---------------------------------------------------------------------------

void SlateInputRouter::SetFocusedWidget(const std::shared_ptr<SWidget>& widget)
{
    auto previous = m_Focused.lock();
    if (previous == widget) return;

    if (previous)
    {
        previous->SetKeyboardFocus(false);
        previous->OnFocusLost();
    }

    if (widget && widget->IsFocusable())
    {
        widget->SetKeyboardFocus(true);
        widget->OnFocusGained();
        m_Focused = widget;
    }
    else
    {
        m_Focused.reset();
    }
}

// ---------------------------------------------------------------------------
// Mouse
// ---------------------------------------------------------------------------

void SlateInputRouter::ProcessMouse(const std::shared_ptr<SWidget>& root,
                                    const Vector2& mousePosition,
                                    bool isOverWindow,
                                    bool leftButtonDown,
                                    float wheelDelta,
                                    bool rightButtonDown)
{
    // A modal widget swallows all input; the rest of the tree is ignored.
    const std::shared_ptr<SWidget> modal = SlateApplication::Get().GetModal();
    const std::shared_ptr<SWidget> treeRoot = modal ? modal : root;

    if (!treeRoot)
    {
        Reset();
        return;
    }

    std::shared_ptr<SWidget> hovered;
    if (isOverWindow)
    {
        HitTestRecursive(treeRoot, treeRoot->GetCachedGeometry(), mousePosition, hovered);
    }

    // ---- hover enter / leave ------------------------------------------------
    auto previouslyHovered = m_Hovered.lock();
    if (previouslyHovered != hovered)
    {
        if (previouslyHovered) previouslyHovered->SetHovered(false);
        if (hovered)           hovered->SetHovered(true);
        m_Hovered = hovered;
    }
    if (hovered) hovered->OnMouseMove(mousePosition);

    // Drag support: while the left button is held, the widget that took the
    // press keeps receiving moves even after the cursor leaves it (column
    // resizing / reordering would otherwise stall at the widget edge).
    auto pressedDuringDrag = m_Pressed.lock();
    if (pressedDuringDrag && pressedDuringDrag != hovered)
        pressedDuringDrag->OnMouseMove(mousePosition);

    // ---- wheel --------------------------------------------------------------
    if (wheelDelta != 0.0f && hovered)
    {
        // Deepest first, then bubble up to the root.
        for (SWidget* widget = hovered.get(); widget != nullptr; widget = widget->GetParent())
        {
            if (widget->OnMouseWheel(mousePosition, wheelDelta).IsHandled()) break;
        }
    }

    // ---- left button --------------------------------------------------------
    if (leftButtonDown && !m_PrevLeftDown)
    {
        if (hovered)
        {
            hovered->OnMouseButtonDown(mousePosition, 0);
            m_Pressed = hovered;
            SetFocusedWidget(hovered);

            if (IsDoubleClick(hovered, 0, mousePosition))
            {
                // Second press: bubble it exactly like the wheel event does.
                for (SWidget* widget = hovered.get(); widget != nullptr; widget = widget->GetParent())
                {
                    if (widget->OnMouseButtonDoubleClick(mousePosition, 0).IsHandled()) break;
                }
                m_LastClicked.reset();
                m_LastClickButton = -1;
            }
            else
            {
                m_LastClicked     = hovered;
                m_LastClickButton = 0;
                m_LastClickTimeMs = NowMilliseconds();
                m_LastClickPos    = mousePosition;
            }
        }
    }
    else if (!leftButtonDown && m_PrevLeftDown)
    {
        auto pressed = m_Pressed.lock();
        if (pressed) pressed->OnMouseButtonUp(mousePosition, 0);
        m_Pressed.reset();
    }

    // ---- right button -------------------------------------------------------
    if (rightButtonDown && !m_PrevRightDown)
    {
        if (hovered) hovered->OnMouseButtonDown(mousePosition, 1);
    }
    else if (!rightButtonDown && m_PrevRightDown)
    {
        if (hovered) hovered->OnMouseButtonUp(mousePosition, 1);
    }

    m_PrevLeftDown  = leftButtonDown;
    m_PrevRightDown = rightButtonDown;
    m_LastMousePos  = mousePosition;
    m_HasMousePos   = true;
}

// ---------------------------------------------------------------------------
// Double-click detection
// ---------------------------------------------------------------------------

bool SlateInputRouter::IsDoubleClick(const std::shared_ptr<SWidget>& widget,
                                     int button, const Vector2& position) const
{
    if (m_LastClickButton != button) return false;
    if (m_LastClicked.lock() != widget) return false;
    if (!widget) return false;
    if (NowMilliseconds() - m_LastClickTimeMs > m_DoubleClickTimeMs) return false;

    const float dx = position.x - m_LastClickPos.x;
    const float dy = position.y - m_LastClickPos.y;
    return std::sqrt(dx * dx + dy * dy) <= m_DoubleClickDistance;
}

// ---------------------------------------------------------------------------
// Keyboard
// ---------------------------------------------------------------------------

void SlateInputRouter::ProcessKey(uint32_t keyCode, bool isDown)
{
    auto focused = m_Focused.lock();
    if (focused) focused->OnKeyDown(keyCode, !isDown);
}

void SlateInputRouter::ProcessCharacter(uint32_t codepoint)
{
    auto focused = m_Focused.lock();
    if (focused) focused->OnKeyChar(codepoint);
}

// ---------------------------------------------------------------------------

void SlateInputRouter::Reset()
{
    auto hovered = m_Hovered.lock();
    if (hovered) hovered->SetHovered(false);

    m_Hovered.reset();
    m_Pressed.reset();
    m_Focused.reset();
    m_PrevLeftDown  = false;
    m_PrevRightDown = false;
    m_HasMousePos   = false;

    m_LastClicked.reset();
    m_LastClickButton = -1;
}

} // namespace ZSlate
