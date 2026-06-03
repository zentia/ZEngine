#pragma once

#include "Runtime/Slate/Core/SlateGeometry.h"
#include "Runtime/Slate/Core/SlateKeys.h"

#include <memory>
#include <vector>

namespace ZSlate
{
class SWidget;
struct FDragDropOperation;

// Routes mouse + keyboard input into a ZSlate widget tree (UE Slate
// FSlateApplication path, simplified). Drive it once per frame from the host
// panel with the live input state. Responsibilities:
//   * hit-test against each widget's cached geometry
//   * hover enter/leave along the full hit path
//   * button-down bubbling deepest->root with mouse capture
//   * deliver move events to the captured widget (drag), wheel to the hit path
//   * keyboard focus management + char/key delivery to the focused widget
class SlateInputRouter
{
public:
    // Clear all hover/capture/focus state. Call when the tree is rebuilt so we
    // never dereference freed widgets.
    void Reset();

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

    bool HasKeyboardFocus() const { return m_Focused != nullptr; }

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
    SWidget* GetHoveredWidget() const { return m_HoverPath.empty() ? nullptr : m_HoverPath.back(); }

private:
    void SetFocus(SWidget* widget);
    void EndDrag();

    std::vector<SWidget*> m_HoverPath;
    SWidget* m_Captured {nullptr};
    SWidget* m_Focused {nullptr};
    bool m_PrevLeftDown {false};
    bool m_PrevRightDown {false};
    SWidget* m_RightCaptured {nullptr};

    // Drag-drop gesture state.
    std::vector<SWidget*> m_PressPath;  // hit path captured at left-button-down
    Vector2 m_PressOrigin {0.0f, 0.0f};
    bool m_LeftPressed {false};
    std::shared_ptr<FDragDropOperation> m_DragOp;  // non-null while dragging
    SWidget* m_DragOverTarget {nullptr};
};
}  // namespace ZSlate
