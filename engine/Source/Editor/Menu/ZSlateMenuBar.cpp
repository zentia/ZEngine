#include "ZSlateMenuBar.h"

#include "Runtime/Core/Base/Macro.h"
#include "ZSlate/Widgets/SMenu.h"
#include "Runtime/UI/Render/BatchedUIRenderer.h"

#include <algorithm>

namespace ZSlate
{
namespace
{
    constexpr float kTitleFontSize = 14.0f;
    constexpr float kTitlePadX = 10.0f;  // per side, * scale

    // ZSlate::UIColor == Vector4 (same underlying type as ::ZSlate::UIColor).
    // No :: prefix — we want ZSlate::UIColor so ISlateRenderer methods accept them.
    const ZSlate::UIColor kBarBg         {0.12f, 0.12f, 0.14f, 1.0f};
    const ZSlate::UIColor kTitleText     {0.88f, 0.89f, 0.92f, 1.0f};
    const ZSlate::UIColor kTitleHighlight {0.26f, 0.40f, 0.62f, 1.0f};
    const ZSlate::UIColor kTitleHover    {0.24f, 0.25f, 0.29f, 1.0f};

    // Convert ZSlate::UIRect -> ::UIRect (global, used by BatchedUIRenderer).
    // ZSlate::UIRect has fields {x, y, w, h}; ::UIRect has {x, y, width, height}.
    static ::UIRect ToEngineRect(const UIRect& r) {
        return ::UIRect(r.x, r.y, r.w, r.h);
    }
}  // namespace

void ZSlateEditorMenuBar::SetMenus(std::vector<TopMenu> menus)
{
    // Re-setting the menu list invalidates any open dropdown (builders may now
    // point at a different title set).
    CloseAll();
    m_Menus = std::move(menus);
}

void ZSlateEditorMenuBar::CloseAll()
{
    m_ActiveIndex = -1;
    m_Popup.Close();
}

void ZSlateEditorMenuBar::OpenMenu(int index, float scale)
{
    m_ActiveIndex = index;

    // Anchor the dropdown at the bottom-left of the clicked title. m_TitleRects
    // is rebuilt at the top of Render() before any OpenMenu() call, so the index
    // is valid here.
    Vector2 anchor(0.0f, 0.0f);
    if (index >= 0 && index < static_cast<int>(m_TitleRects.size()))
    {
        const UIRect& tr = m_TitleRects[index];
        // ZSlate::UIRect uses .w/.h (not .width/.height)
        anchor = Vector2(tr.x, tr.y + tr.h);
    }

    DropdownBuilder build = (index >= 0 && index < static_cast<int>(m_Menus.size())) ? m_Menus[index].build
                                                                                     : DropdownBuilder();
    m_Popup.Open(anchor, scale, [build](SMenu& menu, float s) {
        if (build)
            build(menu, s);
    });
}

bool ZSlateEditorMenuBar::Render(ISlateRenderer& renderer,
                                 const UIRect& bar_rect,
                                 float scale,
                                 const Vector2& mouse,
                                 bool left_down,
                                 const UIRect& viewport_rect)
{
    const bool left_edge = left_down && !m_PrevLeftDown;
    const float font = kTitleFontSize * scale;
    const float pad = kTitlePadX * scale;

    // ---- Lay out + paint the title strip -----------------------------------
    // bar_rect is ZSlate::UIRect; ISlateRenderer expects ZSlate::UIRect.
    // kBarBg is ::ZSlate::UIColor (from anonymous namespace); ISlateRenderer::DrawQuad
    // expects ZSlate::UIColor. They're both Vector4 -- pass directly.
    renderer.DrawQuad(bar_rect, kBarBg);

    m_TitleRects.clear();
    m_TitleRects.reserve(m_Menus.size());
    float x = bar_rect.x + pad;
    for (int i = 0; i < static_cast<int>(m_Menus.size()); ++i)
    {
        // ISlateRenderer::MeasureText has 2 params (text, font_size).
        const Vector2 sz = renderer.MeasureText(m_Menus[i].title, font);
        const float w = sz.x + pad * 2.0f;
        // ZSlate::UIRect uses .h (not .height)
        const UIRect rect(x, bar_rect.y, w, bar_rect.h);
        m_TitleRects.push_back(rect);
        x += w;
    }

    int hovered_title = -1;
    for (int i = 0; i < static_cast<int>(m_TitleRects.size()); ++i)
    {
        const UIRect& rect = m_TitleRects[i];
        // ZSlate::UIRect uses .w/.h (not .width/.height)
        const bool hover = mouse.x >= rect.x && mouse.x < rect.x + rect.w && mouse.y >= rect.y &&
                           mouse.y < rect.y + rect.h;
        if (hover)
            hovered_title = i;

        if (i == m_ActiveIndex)
            renderer.DrawQuad(rect, kTitleHighlight);
        else if (hover)
            renderer.DrawQuad(rect, kTitleHover);

        // ISlateRenderer::DrawText: pass ZSlate::UIRect + ZSlate::UIColor.
        renderer.DrawTextLabel(rect, m_Menus[i].title, font, kTitleText,
                         TextAnchor::MiddleCenter, TextWrapMode::NoWrap, nullptr);
    }

    // ---- Title interaction --------------------------------------------------
    if (left_edge && hovered_title >= 0)
    {
        if (m_ActiveIndex == hovered_title)
            CloseAll();
        else
            OpenMenu(hovered_title, scale);
    }
    else if (m_ActiveIndex >= 0 && hovered_title >= 0 && hovered_title != m_ActiveIndex)
    {
        // Hover-switch between top-level menus while a dropdown is open.
        OpenMenu(hovered_title, scale);
    }

    // ---- Paint + route the active dropdown chain ---------------------------
    bool over_menu = false;
    if (m_ActiveIndex >= 0 && m_Popup.IsOpen())
    {
        over_menu = m_Popup.Render(renderer, mouse, left_down, 0.0f, viewport_rect, /*base_layer=*/1,
                                   /*auto_close=*/false);
        // An item fired inside the popup closes it; sync the title highlight.
        if (!m_Popup.IsOpen())
            m_ActiveIndex = -1;
    }

    // ---- Outside click closes everything -----------------------------------
    // ZSlate::UIRect uses .w/.h (not .width/.height)
    const bool over_bar = mouse.x >= bar_rect.x && mouse.x < bar_rect.x + bar_rect.w &&
                          mouse.y >= bar_rect.y && mouse.y < bar_rect.y + bar_rect.h;
    if (left_edge && hovered_title < 0 && !over_bar && !over_menu)
        CloseAll();

    m_PrevLeftDown = left_down;
    return over_bar || over_menu;
}
}  // namespace ZSlate
