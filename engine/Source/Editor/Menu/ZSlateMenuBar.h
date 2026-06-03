#pragma once

// ----------------------------------------------------------------------------
// ZSlateEditorMenuBar -- native ZSlate replacement for the editor's ImGui main
// menu bar (File / Window / Edit / ...). Paints the horizontal title strip plus
// the active dropdown chain into the shared BatchedUIRenderer (editor overlay)
// and routes mouse input through SlateInputRouter, one router per open level.
//
// Unlike ImGui's BeginMenu/BeginMenuBar (which clamps popups to the monitor work
// area and clips without a scrollbar), the dropdown is a ZSlate SMenu painted in
// the foreground overlay layer -- it can grow as tall as it needs and is
// repositioned to stay on-screen, so long menus (Window) never get clipped.
//
// Hosting model mirrors ZSlateConsoleWindow's popup: the host (MenuController)
// builds a dropdown lazily on open via a per-title builder callback, then calls
// Render() once per frame with the live mouse state. Nested submenus are managed
// as a stack; a submenu opens while its parent row is hovered and stays open
// while the cursor is over the child popup (diagonal-navigation friendly).
// ----------------------------------------------------------------------------

#include "Editor/Menu/ZSlatePopupMenu.h"  // shared dropdown-chain implementation
#include "Runtime/Slate/Core/SlateGeometry.h"
#include "Runtime/UI/Core/UITypes.h"  // UIRect

#include <functional>
#include <string>
#include <vector>

class BatchedUIRenderer;

namespace ZSlate
{
class SMenu;

class ZSlateEditorMenuBar
{
public:
    // Populates a freshly-cleared dropdown SMenu for a top-level title. Called
    // once when the menu opens (so toggle/check state is sampled at open time,
    // matching Unity's rebuild-on-open behaviour).
    using DropdownBuilder = std::function<void(SMenu& menu, float scale)>;

    struct TopMenu
    {
        std::string title;
        DropdownBuilder build;
    };

    void SetMenus(std::vector<TopMenu> menus);

    bool IsOpen() const { return m_ActiveIndex >= 0; }
    void CloseAll();

    // Paint the bar + active dropdown chain and route input for this frame.
    //   renderer      : shared editor overlay renderer (already inside a frame).
    //   bar_rect      : screen-space strip the titles live in (top of viewport).
    //   scale         : editor DPI scale (font + padding multiplier).
    //   mouse         : absolute cursor position (screen px).
    //   left_down     : left mouse button currently held.
    //   viewport_rect : clamp region for dropdowns (usually the main viewport).
    // Returns true if the cursor is over the bar or an open dropdown (the host
    // should then suppress click-through to whatever is underneath).
    bool Render(BatchedUIRenderer& renderer,
                const UIRect& bar_rect,
                float scale,
                const Vector2& mouse,
                bool left_down,
                const UIRect& viewport_rect);

private:
    void OpenMenu(int index, float scale);

    std::vector<TopMenu> m_Menus;
    int m_ActiveIndex {-1};

    // The dropdown chain (root dropdown + nested submenus) is delegated to the
    // shared ZSlatePopupMenu; the bar only owns the horizontal title strip and
    // its open/close/hover-switch interaction (auto_close is disabled so a click
    // that opens a new dropdown does not immediately self-close).
    ZSlatePopupMenu m_Popup;

    std::vector<UIRect> m_TitleRects;  // per-title hit rect (screen, built each frame)
    bool m_PrevLeftDown {false};
};
}  // namespace ZSlate
