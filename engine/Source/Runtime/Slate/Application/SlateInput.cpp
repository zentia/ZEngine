#include "Runtime/Slate/Application/SlateInput.h"

#include "Runtime/Slate/Application/SlateDragDrop.h"
#include "Runtime/Slate/Widgets/SWidget.h"

#include <algorithm>
#include <cmath>

namespace ZSlate
{
namespace
{
    // Movement (in screen px) a press must travel before it becomes a drag.
    constexpr float kDragTriggerDistance = 5.0f;

    // Build the hit path root->deepest, descending into the topmost child that
    // contains the point. Assumes `widget` already contains the point.
    void BuildHitPath(SWidget* widget, const Vector2& point, std::vector<SWidget*>& out_path)
    {
        out_path.push_back(widget);
        for (int i = widget->GetChildCount() - 1; i >= 0; --i)
        {
            const std::shared_ptr<SWidget> child = widget->GetChildAt(i);
            if (child && child->IsHitTestVisible() && child->GetCachedGeometry().IsUnderLocation(point))
            {
                BuildHitPath(child.get(), point, out_path);
                break;
            }
        }
    }

    bool PathContains(const std::vector<SWidget*>& path, SWidget* widget)
    {
        return std::find(path.begin(), path.end(), widget) != path.end();
    }

    float DistanceSquared(const Vector2& a, const Vector2& b)
    {
        const float dx = a.x - b.x;
        const float dy = a.y - b.y;
        return dx * dx + dy * dy;
    }
}  // namespace

void SlateInputRouter::Reset()
{
    m_HoverPath.clear();
    m_Captured = nullptr;
    m_Focused = nullptr;
    m_PrevLeftDown = false;
    m_PrevRightDown = false;
    m_RightCaptured = nullptr;
    m_PressPath.clear();
    m_LeftPressed = false;
    if (m_DragOp != nullptr && GetActiveDragOperation() == m_DragOp)
        SetActiveDragOperation(nullptr);
    m_DragOp = nullptr;
    m_DragOverTarget = nullptr;
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
    if (widget == m_Focused)
        return;
    if (m_Focused != nullptr)
        m_Focused->OnFocusLost();
    m_Focused = widget;
    if (m_Focused != nullptr)
        m_Focused->OnFocusReceived();
}

void SlateInputRouter::EndDrag()
{
    if (m_DragOverTarget != nullptr)
    {
        m_DragOverTarget->OnDragLeave(m_DragOp);
        m_DragOverTarget = nullptr;
    }
    // Stop publishing this gesture to the cross-window channel, but only if we
    // are the router that owns the currently-published op (another router may
    // have started a new drag in the meantime).
    if (GetActiveDragOperation() == m_DragOp)
        SetActiveDragOperation(nullptr);
    m_DragOp = nullptr;
    m_PressPath.clear();
    m_LeftPressed = false;
}

void SlateInputRouter::ProcessMouse(const std::shared_ptr<SWidget>& root,
                                    const Vector2& mouse_screen,
                                    bool is_over_host,
                                    bool left_down,
                                    float wheel_delta,
                                    bool right_down)
{
    std::vector<SWidget*> new_path;
    if (root && is_over_host && root->IsHitTestVisible() && root->GetCachedGeometry().IsUnderLocation(mouse_screen))
    {
        BuildHitPath(root.get(), mouse_screen, new_path);
    }

    // ---- Drag-drop gesture in flight: route to drop targets, not normal hover.
    if (m_DragOp != nullptr)
    {
        // Find the deepest widget that accepts the operation as a drop target.
        SWidget* target = nullptr;
        for (auto it = new_path.rbegin(); it != new_path.rend(); ++it)
        {
            if ((*it)->OnDragOver(mouse_screen, m_DragOp).IsHandled())
            {
                target = *it;
                break;
            }
        }
        if (target != m_DragOverTarget)
        {
            if (m_DragOverTarget != nullptr)
                m_DragOverTarget->OnDragLeave(m_DragOp);
            if (target != nullptr)
                target->OnDragEnter(m_DragOp);
            m_DragOverTarget = target;
        }

        if (!left_down && m_PrevLeftDown)
        {
            if (m_DragOverTarget != nullptr)
                m_DragOverTarget->OnDrop(mouse_screen, m_DragOp);
            EndDrag();
        }

        m_PrevLeftDown = left_down;
        m_PrevRightDown = right_down;
        return;
    }

    // ---- Hover enter/leave: every widget under the cursor is hovered.
    for (SWidget* widget : m_HoverPath)
    {
        if (!PathContains(new_path, widget))
            widget->OnMouseLeave();
    }
    for (SWidget* widget : new_path)
    {
        if (!PathContains(m_HoverPath, widget))
            widget->OnMouseEnter();
    }
    m_HoverPath = new_path;

    // ---- Drag detection: a held left press that travels far enough asks the
    // press path (deepest -> root) for a drag operation.
    if (left_down && m_LeftPressed && DistanceSquared(mouse_screen, m_PressOrigin) > kDragTriggerDistance * kDragTriggerDistance)
    {
        for (auto it = m_PressPath.rbegin(); it != m_PressPath.rend(); ++it)
        {
            std::shared_ptr<FDragDropOperation> op = (*it)->OnDragDetected(mouse_screen);
            if (op != nullptr)
            {
                // Starting a drag steals capture from any pressed widget.
                if (m_Captured != nullptr)
                {
                    m_Captured->OnMouseCaptureLost();
                    m_Captured = nullptr;
                }
                m_DragOp = op;
                // Publish to the cross-window channel so other windows' routers
                // and non-widget hosts (the Scene viewport) can observe the drag.
                SetActiveDragOperation(op);
                m_PrevLeftDown = left_down;
                m_PrevRightDown = right_down;
                return;
            }
        }
    }

    // ---- Captured widget keeps receiving moves (slider drag, etc.).
    if (m_Captured != nullptr)
        m_Captured->OnMouseMove(mouse_screen);

    // ---- Wheel: bubble deepest -> root until handled.
    if (wheel_delta != 0.0f)
    {
        for (auto it = new_path.rbegin(); it != new_path.rend(); ++it)
        {
            if ((*it)->OnMouseWheel(mouse_screen, wheel_delta).IsHandled())
                break;
        }
    }

    // ---- Left button edges.
    if (left_down && !m_PrevLeftDown)
    {
        // Remember the press so drag detection has a path + origin to work from.
        m_PressPath = new_path;
        m_PressOrigin = mouse_screen;
        m_LeftPressed = true;

        // Focus follows the click: deepest focusable in the path, or clear.
        SWidget* focus_target = nullptr;
        for (auto it = new_path.rbegin(); it != new_path.rend(); ++it)
        {
            if ((*it)->SupportsKeyboardFocus())
            {
                focus_target = *it;
                break;
            }
        }
        SetFocus(focus_target);

        // Button-down edge: bubble deepest -> root until a widget handles it.
        for (auto it = new_path.rbegin(); it != new_path.rend(); ++it)
        {
            const FReply reply = (*it)->OnMouseButtonDown(mouse_screen, 0);
            if (reply.IsHandled())
            {
                m_Captured = reply.ShouldCaptureMouse() ? reply.GetMouseCaptor() : *it;
                break;
            }
        }
    }
    else if (!left_down && m_PrevLeftDown)
    {
        m_LeftPressed = false;
        m_PressPath.clear();

        // Button-up edge: deliver to the captor if any, else bubble the hit path.
        if (m_Captured != nullptr)
        {
            m_Captured->OnMouseButtonUp(mouse_screen, 0);
            m_Captured = nullptr;
        }
        else
        {
            for (auto it = new_path.rbegin(); it != new_path.rend(); ++it)
            {
                if ((*it)->OnMouseButtonUp(mouse_screen, 0).IsHandled())
                    break;
            }
        }
    }

    // ---- Right button edges (context menus). Independent capture so the up
    // edge reaches the same widget the down edge landed on.
    if (right_down && !m_PrevRightDown)
    {
        for (auto it = new_path.rbegin(); it != new_path.rend(); ++it)
        {
            const FReply reply = (*it)->OnMouseButtonDown(mouse_screen, 1);
            if (reply.IsHandled())
            {
                m_RightCaptured = reply.ShouldCaptureMouse() ? reply.GetMouseCaptor() : *it;
                break;
            }
        }
    }
    else if (!right_down && m_PrevRightDown)
    {
        if (m_RightCaptured != nullptr)
        {
            m_RightCaptured->OnMouseButtonUp(mouse_screen, 1);
            m_RightCaptured = nullptr;
        }
        else
        {
            for (auto it = new_path.rbegin(); it != new_path.rend(); ++it)
            {
                if ((*it)->OnMouseButtonUp(mouse_screen, 1).IsHandled())
                    break;
            }
        }
    }

    m_PrevLeftDown = left_down;
    m_PrevRightDown = right_down;
}

void SlateInputRouter::ProcessChar(unsigned int codepoint)
{
    if (m_Focused != nullptr)
        m_Focused->OnKeyChar(codepoint);
}

void SlateInputRouter::ProcessKey(EKey key)
{
    if (m_Focused != nullptr)
        m_Focused->OnKeyDown(key);
}
}  // namespace ZSlate
