#include "ZSlatePopupMenu.h"

#include "Runtime/Slate/Widgets/SMenu.h"

#include <algorithm>

namespace ZSlate
{
void ZSlatePopupMenu::Open(const Vector2& anchor, float scale, const Builder& builder)
{
    m_Open = true;
    m_Anchor = anchor;
    m_Stack.clear();
    m_Routers.clear();
    m_StackGeom.clear();

    auto menu = std::make_shared<SMenu>();
    if (builder)
        builder(*menu, scale);

    m_Stack.push_back(menu);
    m_Routers.emplace_back();
    // The popup typically opens on a right-button release; the next Render frame
    // sees a clean left-button state, so start with no pending edge.
    m_PrevLeftDown = false;
}

void ZSlatePopupMenu::Close()
{
    m_Open = false;
    m_Stack.clear();
    m_Routers.clear();
    m_StackGeom.clear();
}

bool ZSlatePopupMenu::Contains(const FGeometry& geom, const Vector2& p)
{
    return geom.IsUnderLocation(p);
}

bool ZSlatePopupMenu::Render(UIRenderer& renderer,
                             const Vector2& mouse,
                             bool left_down,
                             float wheel,
                             const UIRect& viewport_rect,
                             int base_layer,
                             bool auto_close)
{
    if (!m_Open || m_Stack.empty())
    {
        m_PrevLeftDown = left_down;
        return false;
    }

    const bool left_edge = left_down && !m_PrevLeftDown;

    // ---- Paint the active popup chain --------------------------------------
    m_StackGeom.assign(m_Stack.size(), FGeometry());
    for (size_t i = 0; i < m_Stack.size(); ++i)
    {
        m_Stack[i]->CacheDesiredSize();
        const Vector2 size = m_Stack[i]->GetDesiredSize();

        float mx = 0.0f;
        float my = 0.0f;
        if (i == 0)
        {
            mx = m_Anchor.x;
            my = m_Anchor.y;
        }
        else
        {
            const FGeometry anchor = m_Stack[i - 1]->GetSubMenuAnchor(m_Stack[i]);
            mx = anchor.AbsolutePosition.x + anchor.LocalSize.x;
            my = anchor.AbsolutePosition.y;
        }

        // Clamp on-screen so the popup never gets clipped off the viewport.
        mx = std::min(mx, viewport_rect.x + viewport_rect.width - size.x);
        my = std::min(my, viewport_rect.y + viewport_rect.height - size.y);
        mx = std::max(mx, viewport_rect.x);
        my = std::max(my, viewport_rect.y);

        const FGeometry geom(Vector2(mx, my), size);
        m_StackGeom[i] = geom;

        FPaintContext ctx;
        ctx.Renderer = &renderer;
        ctx.LayerId = base_layer + static_cast<int>(i);
        m_Stack[i]->Paint(ctx, geom);
    }

    // ---- Route input per level; only the hovered level sees presses --------
    bool over_menu = false;
    for (size_t i = 0; i < m_Stack.size(); ++i)
    {
        const bool over = Contains(m_StackGeom[i], mouse);
        if (over)
            over_menu = true;
        if (i < m_Routers.size())
            m_Routers[i].ProcessMouse(m_Stack[i], mouse, over, left_down, over ? wheel : 0.0f);
    }

    // An item fired -> tear the whole chain down.
    for (const auto& menu : m_Stack)
    {
        if (menu->IsCloseRequested())
        {
            Close();
            m_PrevLeftDown = left_down;
            return over_menu;
        }
    }

    // ---- Submenu open/close management (for next frame) --------------------
    // Close levels the cursor has navigated away from. A submenu stays open
    // while the cursor is over it (or a deeper level) OR while the parent row
    // that opened it is still hovered (diagonal-navigation friendly).
    for (int k = static_cast<int>(m_Stack.size()) - 1; k >= 1; --k)
    {
        bool cursor_in_subtree = false;
        for (int j = k; j < static_cast<int>(m_StackGeom.size()); ++j)
        {
            if (Contains(m_StackGeom[j], mouse))
            {
                cursor_in_subtree = true;
                break;
            }
        }
        const bool parent_row_hovered = (m_Stack[k - 1]->GetHoveredSubMenu() == m_Stack[k]);
        if (!cursor_in_subtree && !parent_row_hovered)
        {
            m_Stack.resize(k);
            m_Routers.resize(k);
            m_StackGeom.resize(k);
        }
    }

    // Open a submenu whose row is hovered but not yet shown.
    for (size_t i = 0; i < m_Stack.size(); ++i)
    {
        const std::shared_ptr<SMenu> child = m_Stack[i]->GetHoveredSubMenu();
        if (child && (i + 1 >= m_Stack.size() || m_Stack[i + 1] != child))
        {
            m_Stack.resize(i + 1);
            m_Routers.resize(i + 1);
            m_Stack.push_back(child);
            m_Routers.emplace_back();
            break;
        }
    }

    // ---- Outside click closes the popup ------------------------------------
    // (skipped when the host owns dismissal -- see auto_close in the header.)
    if (auto_close && left_edge && !over_menu)
        Close();

    m_PrevLeftDown = left_down;
    return over_menu;
}
}  // namespace ZSlate
