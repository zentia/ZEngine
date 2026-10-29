#include "ZSlate/Application/SlateInput.h"

#include "ZSlate/Application/SlateDragDrop.h"
#include "ZSlate/Widgets/SWidget.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace ZSlate
{
    // Static thread-local modifier state (accessed by widgets via IsCtrlDown() etc.)
    thread_local bool SlateInputRouter::s_CtrlDown = false;
    thread_local bool SlateInputRouter::s_ShiftDown = false;
    thread_local bool SlateInputRouter::s_AltDown = false;
    thread_local std::weak_ptr<SWidget> SlateInputRouter::s_Popup;

void SlateInputRouter::OpenPopup(const std::shared_ptr<SWidget>& owner)
{
    if (owner && dynamic_cast<ISlatePopup*>(owner.get()))
        s_Popup = owner;
}

void SlateInputRouter::ClosePopup(const SWidget* owner)
{
    if (s_Popup.lock().get() == owner)
        s_Popup.reset();
}

std::shared_ptr<SWidget> SlateInputRouter::GetActivePopupOwner()
{
    auto owner = s_Popup.lock();
    auto* popup = owner ? dynamic_cast<ISlatePopup*>(owner.get()) : nullptr;
    if (!popup || !popup->IsPopupOpen())
    {
        s_Popup.reset();
        return nullptr;
    }
    return owner;
}

ISlatePopup* SlateInputRouter::GetActivePopup()
{
    auto owner = GetActivePopupOwner();
    return owner ? dynamic_cast<ISlatePopup*>(owner.get()) : nullptr;
}

namespace
{
    // Movement (in screen px) a press must travel before it becomes a drag.
    constexpr float kDragTriggerDistance = 5.0f;

    // Monotonic clock for double-click detection.
    double NowMilliseconds()
    {
        using clock = std::chrono::steady_clock;
        return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
    }

    // Build the hit path root->deepest, descending into the topmost child that
    // contains the point. Assumes `widget` already contains the point.
    // Pushes std::weak_ptr<SWidget> so the path remains valid even if widgets
    // are later destroyed (UE: FWidgetPath stores TArray<TWeakPtr<SWidget>>).
    void BuildHitPath(const std::shared_ptr<SWidget>& widget, const Vector2& point,
                      std::vector<std::weak_ptr<SWidget>>& out_path)
    {
        out_path.push_back(widget);
        for (int i = widget->GetChildCount() - 1; i >= 0; --i)
        {
            const std::shared_ptr<SWidget> child = widget->GetChildAt(i);
            if (child && child->IsHitTestVisible() && child->GetCachedGeometry().IsUnderLocation(point))
            {
                BuildHitPath(child, point, out_path);
                break;
            }
        }
    }

    // Check if a weak_ptr path contains a widget (by raw pointer comparison after locking).
    bool PathContains(const std::vector<std::weak_ptr<SWidget>>& path,
                      const std::shared_ptr<SWidget>& widget)
    {
        if (!widget) return false;
        for (const auto& wp : path)
        {
            if (auto sp = wp.lock())
            {
                if (sp.get() == widget.get())
                    return true;
            }
        }
        return false;
    }

    float DistanceSquared(const Vector2& a, const Vector2& b)
    {
        const float dx = a.x - b.x;
        const float dy = a.y - b.y;
        return dx * dx + dy * dy;
    }
}  // namespace

// ---- weak_ptr helpers ----

SWidget* SlateInputRouter::Lock(const std::weak_ptr<SWidget>& wp)
{
    auto sp = wp.lock();
    return sp ? sp.get() : nullptr;
}

std::vector<SWidget*> SlateInputRouter::LockPath(const std::vector<std::weak_ptr<SWidget>>& path)
{
    std::vector<SWidget*> result;
    result.reserve(path.size());
    for (const auto& wp : path)
    {
        if (auto sp = wp.lock())
            result.push_back(sp.get());
    }
    return result;
}

SWidget* SlateInputRouter::GetHoveredWidget() const
{
    if (m_HoverPath.empty()) return nullptr;
    return Lock(m_HoverPath.back());
}

SWidget* SlateInputRouter::GetKeyboardFocusedWidget() const
{
    return Lock(m_Focused);
}

bool SlateInputRouter::HasKeyboardFocus() const
{
    return !m_Focused.expired();
}

// ---- Reset / focus ----

void SlateInputRouter::Reset()
{
    ClearKeyboardFocus();
    ResetPointerState();
}

void SlateInputRouter::ResetPointerState()
{
    for (const auto& weak : m_HoverPath)
        if (auto widget = weak.lock()) widget->OnMouseLeave();
    if (auto widget = m_Captured.lock()) widget->OnMouseCaptureLost();
    if (auto widget = m_RightCaptured.lock()) widget->OnMouseCaptureLost();
    m_HoverPath.clear();
    m_Captured.reset();
    m_PrevLeftDown = false;
    m_PrevRightDown = false;
    m_RightCaptured.reset();
    m_PressPath.clear();
    m_LeftPressed = false;
    if (m_DragOp != nullptr && GetActiveDragOperation() == m_DragOp)
        SetActiveDragOperation(nullptr);
    m_DragOp = nullptr;
    m_LastObservedDragOp.reset();
    m_DragOverTarget.reset();
}

void SlateInputRouter::SetKeyboardFocus(SWidget* widget)
{
    SetFocus(widget);
}

void SlateInputRouter::ClearKeyboardFocus()
{
    SetFocus(nullptr);
}

void SlateInputRouter::SetFocus(SWidget* widget)
{
    // To set focus on a widget, we need its shared_ptr. Look up from the hover
    // path or press path, or accept that the caller passes a raw pointer from a
    // widget that is guaranteed to be alive during the call.
    // In most cases, SetFocus is called in button-down processing with a widget
    // from new_path, which is backed by active shared_ptrs from GetChildAt().
    // We store a weak_ptr; if the widget is destroyed, focus auto-clears.
    auto oldFocused = m_Focused.lock();
    if (oldFocused && oldFocused.get() == widget)
        return;
    if (oldFocused)
        oldFocused->OnFocusLost();
    m_Focused.reset();
    // The caller must ensure `widget` outlives this call (it's from the current
    // tree). We can't obtain a weak_ptr from a raw pointer directly, so we
    // accept a raw pointer for focus (same as UE's approach for keyboard focus).
    // This is safe because SetKeyboardFocusWidget validates before setting, and
    // OnFocusLost is only called when we HAVE a valid shared_ptr from lock().
    // NOTE: m_Focused remains as weak_ptr but we don't set it to a raw pointer;
    // we need to rethink this for true safety. For now, maintain backwards compat
    // by storing nullptr in m_Focused when clearing, and use oldFocused for the
    // OnFocusLost notification.
    (void)widget; // raw pointer focus is deprecated; use keyboard-focus API
}

void SlateInputRouter::SetKeyboardFocusWidget(SWidget* widget)
{
    if (widget && !widget->SupportsKeyboardFocus())
        return;
    // We can't create a weak_ptr from a raw pointer. The caller must ensure
    // the widget is alive. For safety, we don't store dangling focus — the
    // widget tree must handle focus via ProcessMouse button-down.
    // This is a known limitation: raw-pointer focus will not auto-clear on
    // widget destruction. Use the keyboard navigation API instead.
    auto oldFocused = m_Focused.lock();
    if (oldFocused)
        oldFocused->OnFocusLost();
    m_Focused.reset();  // effectively clears focus since we can't store raw ptr as weak
}

void SlateInputRouter::EndDrag()
{
    if (auto target = m_DragOverTarget.lock())
    {
        target->OnDragLeave(m_DragOp);
        m_DragOverTarget.reset();
    }
    if (GetActiveDragOperation() == m_DragOp)
        SetActiveDragOperation(nullptr);
    m_DragOp = nullptr;
    m_LastObservedDragOp.reset();
    m_PressPath.clear();
    m_LeftPressed = false;
}

// ---- Main input routing (UE: FSlateApplication::RoutePointerMoveEvent) ----

bool SlateInputRouter::IsDoubleClick(const std::shared_ptr<SWidget>& widget,
                                     const Vector2& mouse_screen) const
{
    if (!widget) return false;
    if (m_LastClicked.lock() != widget) return false;
    if (NowMilliseconds() - m_LastClickTimeMs > m_DoubleClickTimeMs) return false;

    const float dx = mouse_screen.x - m_LastClickPos.x;
    const float dy = mouse_screen.y - m_LastClickPos.y;
    return std::sqrt(dx * dx + dy * dy) <= m_DoubleClickDistance;
}

void SlateInputRouter::ProcessMouse(const std::shared_ptr<SWidget>& root,
                                    const Vector2& mouse_screen,
                                    bool is_over_host,
                                    bool left_down,
                                    float wheel_delta,
                                    bool right_down)
{
    // ---- Open popup swallows the pointer (UE menu stack) -------------------
    // The owner usually lives inside `root`, but its dropdown extends beyond
    // its own geometry, so hit-testing the tree cannot reach it. Hover is
    // dropped for the tree underneath, and the gesture that opened the popup
    // is ended so its button-up does not land on a stale capture.
    if (const std::shared_ptr<SWidget> owner = GetActivePopupOwner())
    {
        if (!m_HoverPath.empty() || !m_Captured.expired() || !m_RightCaptured.expired())
        {
            for (const auto& weak : m_HoverPath)
                if (auto widget = weak.lock()) widget->OnMouseLeave();
            m_HoverPath.clear();
            m_Captured.reset();
            m_RightCaptured.reset();
            m_PressPath.clear();
            m_LeftPressed = false;
        }
        dynamic_cast<ISlatePopup*>(owner.get())->ProcessPopupMouse(mouse_screen, left_down, wheel_delta);
        m_PrevLeftDown = left_down;
        m_PrevRightDown = right_down;
        return;
    }

    // Build new hit path using weak_ptrs (UE: TWeakPtr<SWidget>).
    std::vector<std::weak_ptr<SWidget>> new_path;
    if (root && is_over_host && root->IsHitTestVisible() && root->GetCachedGeometry().IsUnderLocation(mouse_screen))
    {
        BuildHitPath(root, mouse_screen, new_path);
    }

    // ---- Drag-drop gesture in flight ----
    // Own drag first, else an op published by another window (cross-window drop).
    std::shared_ptr<FDragDropOperation> drag_op = m_DragOp != nullptr ? m_DragOp : GetActiveDragOperation();
    if (drag_op != nullptr)
    {
        m_LastObservedDragOp = drag_op;
    }
    else if (m_LastObservedDragOp != nullptr && (left_down || m_PrevLeftDown))
    {
        // Channel already cleared by the publishing router this frame: deliver
        // from our cached copy so a cross-window drop is not lost to draw order.
        // The release frame (left up, was down) is THE frame that needs it: the
        // source router ends its gesture on that frame, and when its window ticks
        // first the channel is already empty here. Gating this on `left_down` (as
        // it was) dropped every cross-window drop whose target window ticks after
        // the source -- e.g. Hierarchy -> Content Browser "create prefab".
        drag_op = m_LastObservedDragOp;
    }

    if (drag_op != nullptr)
    {
        // Find deepest widget that accepts the operation.
        std::shared_ptr<SWidget> target;
        for (auto it = new_path.rbegin(); it != new_path.rend(); ++it)
        {
            if (auto widget = it->lock())
            {
                if (widget->OnDragOver(mouse_screen, drag_op).IsHandled())
                {
                    target = widget;
                    break;
                }
            }
        }
        auto oldTarget = m_DragOverTarget.lock();
        if (target.get() != oldTarget.get())
        {
            if (oldTarget)
                oldTarget->OnDragLeave(drag_op);
            if (target)
                target->OnDragEnter(drag_op);
            m_DragOverTarget = target;
        }

        if (!left_down && m_PrevLeftDown)
        {
            if (auto dropTarget = m_DragOverTarget.lock())
                dropTarget->OnDrop(mouse_screen, drag_op);
            if (m_DragOp != nullptr)
                EndDrag();
            // Forget both: a stale op must not fire on a later gesture, and a
            // lingering drag-over target would swallow the next drag's
            // OnDragEnter (so its highlight would never turn on again).
            m_LastObservedDragOp.reset();
            m_DragOverTarget.reset();
        }

        m_PrevLeftDown = left_down;
        m_PrevRightDown = right_down;
        return;
    }

    // ---- Hover enter/leave (UE: RoutePointerMoveEvent lines 5752-5818) ----
    // Iterate old hover path, send OnMouseLeave to widgets no longer under cursor.
    // weak_ptrs: expired == widget destroyed → safely skip (UE: .Pin().IsValid()).
    for (const auto& wp : m_HoverPath)
    {
        if (auto widget = wp.lock())
        {
            if (!PathContains(new_path, widget))
                widget->OnMouseLeave();
        }
    }
    // Iterate new path, send OnMouseEnter to widgets newly under cursor.
    for (const auto& wp : new_path)
    {
        if (auto widget = wp.lock())
        {
            if (!PathContains(m_HoverPath, widget))
                widget->OnMouseEnter();
        }
    }
    m_HoverPath = new_path;

    // ---- Drag detection ----
    if (left_down && m_LeftPressed && DistanceSquared(mouse_screen, m_PressOrigin) > kDragTriggerDistance * kDragTriggerDistance)
    {
        for (auto it = m_PressPath.rbegin(); it != m_PressPath.rend(); ++it)
        {
            if (auto widget = it->lock())
            {
                std::shared_ptr<FDragDropOperation> op = widget->OnDragDetected(mouse_screen);
                if (op != nullptr)
                {
                    if (auto cap = m_Captured.lock())
                    {
                        cap->OnMouseCaptureLost();
                        m_Captured.reset();
                    }
                    m_DragOp = op;
                    SetActiveDragOperation(op);
                    m_PrevLeftDown = left_down;
                    m_PrevRightDown = right_down;
                    return;
                }
            }
        }
    }

    // ---- Captured widget receives moves ----
    if (auto cap = m_Captured.lock())
        cap->OnMouseMove(mouse_screen);

    // ---- Pointer move over the hover path ---------------------------------
    // OnMouseEnter/OnMouseLeave cannot tell a widget *where* the cursor is, and
    // the captured widget above used to be the only one that ever got a move.
    // So anything tracking hover off a plain move -- a list row highlight, a
    // tree node, a close button, a hover card -- only worked for as long as a
    // click on it kept the capture alive. Deliver the move to the whole hover
    // path, deepest first (same order as the wheel bubble below).
    const std::shared_ptr<SWidget> captured = m_Captured.lock();
    for (auto it = new_path.rbegin(); it != new_path.rend(); ++it)
    {
        if (auto widget = it->lock())
        {
            // The captured widget already got it above; do not send it twice.
            if (widget.get() != captured.get())
                widget->OnMouseMove(mouse_screen);
        }
    }

    // ---- Wheel: bubble deepest -> root ----
    if (wheel_delta != 0.0f)
    {
        for (auto it = new_path.rbegin(); it != new_path.rend(); ++it)
        {
            if (auto widget = it->lock())
            {
                if (widget->OnMouseWheel(mouse_screen, wheel_delta).IsHandled())
                    break;
            }
        }
    }

    // ---- Left button edges ----
    if (left_down && !m_PrevLeftDown)
    {
        m_PressPath = new_path;
        m_PressOrigin = mouse_screen;
        m_LeftPressed = true;

        // Focus follows click: deepest focusable in the path, or clear.
        {
            auto oldFocused = m_Focused.lock();
            if (oldFocused)
                oldFocused->OnFocusLost();
            m_Focused.reset();

            std::shared_ptr<SWidget> focus_target;
            for (auto it = new_path.rbegin(); it != new_path.rend(); ++it)
            {
                if (auto widget = it->lock())
                {
                    if (widget->SupportsKeyboardFocus())
                    {
                        focus_target = widget;
                        break;
                    }
                }
            }
            if (focus_target)
            {
                m_Focused = focus_target;
                focus_target->OnFocusReceived();
            }
        }

        // Button-down edge: bubble deepest -> root until handled.
        for (auto it = new_path.rbegin(); it != new_path.rend(); ++it)
        {
            if (auto widget = it->lock())
            {
                const FReply reply = widget->OnMouseButtonDown(mouse_screen, 0);
                if (reply.IsHandled())
                {
                    SWidget* captor = reply.ShouldCaptureMouse() ? reply.GetMouseCaptor() : widget.get();
                    // Store weak_ptr to captor. The captor is either in new_path or
                    // specified by the reply. We need its shared_ptr to make a weak_ptr.
                    // Use the widget shared_ptr from the lock.
                    if (captor == widget.get())
                        m_Captured = widget;  // widget is the shared_ptr from lock()
                    else
                        m_Captured.reset();   // external captor, can't track safely
                    break;
                }
            }
        }

        // ---- Double-click -------------------------------------------------
        // UE exposes this as FPointerEvent::IsDoubleClick; we synthesise it here
        // so any widget can opt in through OnMouseButtonDoubleClick.
        const std::shared_ptr<SWidget> deepest =
            new_path.empty() ? nullptr : new_path.back().lock();

        if (deepest && IsDoubleClick(deepest, mouse_screen))
        {
            // Bubble exactly like the button-down edge above.
            for (auto it = new_path.rbegin(); it != new_path.rend(); ++it)
            {
                if (auto widget = it->lock())
                {
                    if (widget->OnMouseButtonDoubleClick(mouse_screen, 0).IsHandled())
                        break;
                }
            }
            m_LastClicked.reset();
        }
        else
        {
            m_LastClicked     = deepest;
            m_LastClickPos    = mouse_screen;
            m_LastClickTimeMs = NowMilliseconds();
        }
    }
    else if (!left_down && m_PrevLeftDown)
    {
        m_LeftPressed = false;
        m_PressPath.clear();

        if (auto cap = m_Captured.lock())
        {
            cap->OnMouseButtonUp(mouse_screen, 0);
            m_Captured.reset();
        }
        else
        {
            for (auto it = new_path.rbegin(); it != new_path.rend(); ++it)
            {
                if (auto widget = it->lock())
                {
                    if (widget->OnMouseButtonUp(mouse_screen, 0).IsHandled())
                        break;
                }
            }
        }
    }

    // ---- Right button edges ----
    if (right_down && !m_PrevRightDown)
    {
        for (auto it = new_path.rbegin(); it != new_path.rend(); ++it)
        {
            if (auto widget = it->lock())
            {
                const FReply reply = widget->OnMouseButtonDown(mouse_screen, 1);
                if (reply.IsHandled())
                {
                    SWidget* captor = reply.ShouldCaptureMouse() ? reply.GetMouseCaptor() : widget.get();
                    if (captor == widget.get())
                        m_RightCaptured = widget;
                    else
                        m_RightCaptured.reset();
                    break;
                }
            }
        }
    }
    else if (!right_down && m_PrevRightDown)
    {
        if (auto rc = m_RightCaptured.lock())
        {
            rc->OnMouseButtonUp(mouse_screen, 1);
            m_RightCaptured.reset();
        }
        else
        {
            for (auto it = new_path.rbegin(); it != new_path.rend(); ++it)
            {
                if (auto widget = it->lock())
                {
                    if (widget->OnMouseButtonUp(mouse_screen, 1).IsHandled())
                        break;
                }
            }
        }
    }

    m_PrevLeftDown = left_down;
    m_PrevRightDown = right_down;
}

void SlateInputRouter::ProcessChar(unsigned int codepoint)
{
    if (auto focused = m_Focused.lock())
        focused->OnKeyChar(codepoint);
}

void SlateInputRouter::ProcessKey(EKey key)
{
    if (auto focused = m_Focused.lock())
        focused->OnKeyDown(key);
}
}  // namespace ZSlate
