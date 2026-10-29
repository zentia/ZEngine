#pragma once

#include "ZSlate/Core/SlateGeometry.h"
#include "ZSlate/Core/SlateKeys.h"

#include <memory>
#include <vector>

namespace ZSlate
{
class SWidget;
struct ISlateRenderer;
struct FDragDropOperation;

// A transient overlay owned by a widget (combo-box dropdown, menu). While one
// is open it is painted above everything by SlateApplication::PaintInto and
// receives all mouse input before the widget tree (UE FSlateApplication menu
// stack, simplified to one level). Implemented by the owning SWidget.
class ISlatePopup
{
public:
    virtual ~ISlatePopup() = default;
    // Return false once the popup has dismissed itself.
    virtual bool IsPopupOpen() const = 0;
    virtual bool ProcessPopupMouse(const Vector2& pos, bool left_down, float wheel) = 0;
    virtual void PaintPopup(ISlateRenderer* renderer, const UIRect& viewport) const = 0;
};

// Routes mouse + keyboard input into a ZSlate widget tree (UE Slate
// FSlateApplication path, simplified). Drive it once per frame from the host
// panel with the live input state.
//
// CRITICAL: All stored widget references use std::weak_ptr<SWidget> (matching
// UE's TWeakPtr<SWidget> pattern in FWidgetPath). Before each use, we lock()
// and check validity. This prevents dangling-pointer crashes when widgets are
// destroyed between frames — no manual Reset() or root-change detection needed.
class SlateInputRouter
{
public:
    // Clear all hover/capture/focus state. Call when the tree is rebuilt so we
    // never dereference freed widgets.
    void Reset();
    // End the current pointer gesture while preserving the focused widget.
    void ResetPointerState();

    // Drop hover-path pointers without clearing keyboard focus or mouse capture.
    // Use when only log-row widgets are destroyed (Console filter rebuild).
    void ClearHoverPath() { m_HoverPath.clear(); }

    SWidget* GetKeyboardFocusedWidget() const;

    // Set keyboard focus to a specific widget. The widget must have
    // SupportsKeyboardFocus() == true (checked by the setter).
    void SetKeyboardFocusWidget(SWidget* widget);

    // `right_down` lets widgets handle right-clicks (context menus). Optional so
    // existing call sites that only care about the left button compile unchanged.
    void ProcessMouse(const std::shared_ptr<SWidget>& root,
                      const Vector2& mouse_screen,
                      bool is_over_host,
                      bool left_down,
                      float wheel_delta,
                      bool right_down = false);

    void ProcessChar(unsigned int codepoint);
    void ProcessKey(EKey key);

    bool HasKeyboardFocus() const;

    // Modifier key state (static, set by host via SetModifierState, read by widgets)
    static bool IsCtrlDown() { return s_CtrlDown; }
    static bool IsShiftDown() { return s_ShiftDown; }
    static bool IsAltDown() { return s_AltDown; }

    // Call this from the host before ProcessKey/ProcessChar to update modifier state.
    static void SetModifierState(bool ctrl, bool shift, bool alt)
    {
        s_CtrlDown = ctrl;
        s_ShiftDown = shift;
        s_AltDown = alt;
    }

    // ---- Popup layer (per-thread, shared by every router on the UI thread) --
    // `owner` must implement ISlatePopup. Opening a popup replaces any other.
    static void OpenPopup(const std::shared_ptr<SWidget>& owner);
    // Closes the popup only if `owner` is the active one.
    static void ClosePopup(const SWidget* owner);
    static std::shared_ptr<SWidget> GetActivePopupOwner();
    static ISlatePopup* GetActivePopup();

    // Programmatically move keyboard focus (e.g. onto a freshly-shown rename box)
    // or clear it. Safe to pass a widget that is part of the routed tree.
    void SetKeyboardFocus(SWidget* widget);
    void ClearKeyboardFocus();

    // True while a drag-drop gesture is in flight (a source returned an op and
    // the button is still held). Hosts can use this to draw a drag decorator.
    bool IsDragging() const { return m_DragOp != nullptr; }
    const std::shared_ptr<FDragDropOperation>& GetDragOperation() const { return m_DragOp; }

    // Deepest widget currently under the cursor (end of the hover path), or null
    // if nothing is hovered. Hosts use this to apply the widget's GetCursor().
    SWidget* GetHoveredWidget() const;

    // ---- Double-click tuning ---------------------------------------------
    // A press counts as the second half of a double-click when it lands on the
    // same widget within this time (ms) and this distance (px) of the previous
    // one. Defaults match the Windows SM_CXDOUBLECLK-ish feel.
    void  SetDoubleClickTime(double milliseconds) { m_DoubleClickTimeMs = milliseconds; }
    double GetDoubleClickTime() const { return m_DoubleClickTimeMs; }
    void  SetDoubleClickDistance(float pixels) { m_DoubleClickDistance = pixels; }
    float GetDoubleClickDistance() const { return m_DoubleClickDistance; }

private:
    // Helper: lock a weak_ptr, return raw pointer or nullptr if expired.
    static SWidget* Lock(const std::weak_ptr<SWidget>& wp);

    // Helper: lock all non-expired weak_ptrs in a path, returning raw pointers.
    static std::vector<SWidget*> LockPath(const std::vector<std::weak_ptr<SWidget>>& path);

    void SetFocus(SWidget* widget);
    void EndDrag();

    // True when this press is the second half of a double-click on `widget`.
    bool IsDoubleClick(const std::shared_ptr<SWidget>& widget, const Vector2& mouse_screen) const;

    // ---- Widget paths use weak_ptr (UE: TWeakPtr<SWidget>) ----
    // Destroyed widgets lock() to nullptr; loops safely skip expired entries.
    std::vector<std::weak_ptr<SWidget>> m_HoverPath;
    std::vector<std::weak_ptr<SWidget>> m_PressPath;

    // Captured / focused widgets also use weak_ptr for safety.
    std::weak_ptr<SWidget> m_Captured;
    std::weak_ptr<SWidget> m_Focused;
    std::weak_ptr<SWidget> m_RightCaptured;
    std::weak_ptr<SWidget> m_DragOverTarget;

    bool m_PrevLeftDown {false};
    bool m_PrevRightDown {false};

    // Drag-drop gesture state.
    Vector2 m_PressOrigin {0.0f, 0.0f};
    bool m_LeftPressed {false};
    std::shared_ptr<FDragDropOperation> m_DragOp;  // non-null while dragging
    // Last op this router observed, including ops published by ANOTHER window.
    // The publishing router clears the process-wide channel on its own release
    // frame, which may run before this router's tick (window draw order); the
    // drop then has to be delivered from this copy -- see SlateDragDrop.h.
    std::shared_ptr<FDragDropOperation> m_LastObservedDragOp;

    // ---- Double-click tracking -------------------------------------------
    std::weak_ptr<SWidget> m_LastClicked;
    Vector2 m_LastClickPos   {0.0f, 0.0f};
    double  m_LastClickTimeMs {0.0};
    double  m_DoubleClickTimeMs {500.0};
    float   m_DoubleClickDistance {5.0f};

    // Static modifier key state (per-thread, set by ProcessKey)
    static thread_local bool s_CtrlDown;
    static thread_local bool s_ShiftDown;
    static thread_local bool s_AltDown;
    static thread_local std::weak_ptr<SWidget> s_Popup;
};
}  // namespace ZSlate
