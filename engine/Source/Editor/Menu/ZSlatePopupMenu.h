#pragma once

// ----------------------------------------------------------------------------
// ZSlatePopupMenu -- reusable anchored popup menu (right-click context menu)
// painted into a ZSlate UIRenderer as a foreground overlay layer and clamped
// on-screen. Extracted from ZSlateEditorMenuBar's dropdown-chain logic so any
// editor window can raise a context menu with the SAME behaviour the main menu
// bar uses: nested submenus managed as a host-side stack (diagonal-navigation
// friendly), outside-click dismiss, and item-fire teardown.
//
// This replaces the per-window inline single-level SMenu plumbing that
// ZSlateHierarchyWindow / ZSlateContentBrowserWindow each duplicated (m_Menu +
// m_MenuOpen + m_MenuInput + Open/Close), and lifts them to full submenu
// support for free.
//
// Renderer-agnostic: Render() takes a base ISlateRenderer&, so the same call
// works for the native BatchedUIRenderer (editor overlay) and the legacy
// SlateImGuiRenderer fallback. The caller is responsible for ensuring the
// renderer is inside a paintable frame (native: the overlay frame is already
// active; legacy: wrap the call in beginFrame()/endFrame() on the foreground
// draw list).
// ----------------------------------------------------------------------------

#include "ZSlate/Application/SlateInput.h"
#include "ZSlate/Backend/SlateUIRendererBackend.h"  // ISlateRenderer
#include "ZSlate/Core/SlateGeometry.h"
#include "Runtime/UI/Core/UITypes.h"  // UIRect

#include <functional>
#include <memory>
#include <vector>

namespace ZSlate
{
class SMenu;

class ZSlatePopupMenu
{
public:
    // Populates a freshly-cleared root SMenu. Runs once when the popup opens, so
    // toggle/check state is sampled at open time (Unity-style rebuild-on-open).
    using Builder = std::function<void(SMenu& menu, float scale)>;

    // Open the popup at an anchor (usually the cursor position). Replaces any
    // currently-open popup. The builder fills the root menu.
    void Open(const Vector2& anchor, float scale, const Builder& builder);
    void Close();
    bool IsOpen() const { return m_Open; }

    // Paint + route the active popup chain for this frame.
    //   renderer      : ZSlate renderer already inside a frame.
    //   mouse         : absolute cursor position (screen px).
    //   left_down     : left mouse button currently held.
    //   wheel         : mouse wheel delta (forwarded to the hovered level).
    //   viewport_rect : clamp region for the popup (usually the main viewport).
    //   base_layer    : overlay LayerId for the root popup (submenus stack above).
    //   auto_close    : when true (default), an outside press closes the popup.
    //                   Set false when the host owns dismissal (e.g. the menu bar
    //                   manages open/close across its title strip, where a click
    //                   that just opened a new dropdown must NOT immediately
    //                   self-close). Item-fire always closes regardless.
    // Returns true if the cursor is over the popup chain (host should swallow
    // click-through). Closes the popup on an outside press or item firing.
    bool Render(ISlateRenderer& renderer,
                const Vector2& mouse,
                bool left_down,
                float wheel,
                const UIRect& viewport_rect,
                int base_layer = 1,
                bool auto_close = true);

private:
    static bool Contains(const FGeometry& geom, const Vector2& p);

    bool m_Open {false};
    Vector2 m_Anchor {0.0f, 0.0f};

    // m_Stack[0] = root popup; m_Stack[i+1] = submenu opened from level i.
    std::vector<std::shared_ptr<SMenu>> m_Stack;
    std::vector<FGeometry> m_StackGeom;       // screen geometry each level painted into
    std::vector<SlateInputRouter> m_Routers;  // one router per stack level

    bool m_PrevLeftDown {false};
};
}  // namespace ZSlate
