#include "Editor/EditorLayout/ZSlateDock/DockHost.h"

#include "Editor/EditorLayout/EditorLayoutWindowIds.h"
#include "Editor/EditorLayout/ZSlateDock/DockTree.h"
#include "Runtime/UI/Render/BatchedUIRenderer.h"

#include <algorithm>
#include <vector>

namespace EditorDock
{
    namespace
    {
        bool RectContains(const UIRect& r, const Vector2& p)
        {
            return p.x >= r.x && p.x <= r.x + r.width && p.y >= r.y && p.y <= r.y + r.height;
        }

        // Width reserved on the active tab for the close ("x") button.
        float CloseButtonWidth(float font) { return font + 8.0f; }

        // Visible tab-region width: the strip minus the scroll-arrow zone (only present
        // when the tabs overflow). Recomputed identically by layout + render.
        float UsableTabWidth(const DockNode& leaf, float strip_width, float arrow_w)
        {
            return leaf.ScrollLeftRect.width > 0.0f ? std::max(1.0f, strip_width - 2.0f * arrow_w) : strip_width;
        }

        // Single source of truth for tab-strip layout: fills leaf.TabRects (index-aligned
        // with leaf.Tabs), the active tab's close-button rect, the scroll-arrow rects (only
        // when overflowing), and clamps leaf.TabScrollOffset. P7: tabs that overflow the
        // strip are reachable by horizontal scroll (wheel + arrow buttons) instead of being
        // dropped, so every tab gets a real rect (the renderer clips the strip).
        void LayoutLeafTabs(DockNode& leaf, BatchedUIRenderer& renderer, float scale, const DockHost::Style& style)
        {
            leaf.TabRects.assign(leaf.Tabs.size(), UIRect());
            leaf.ScrollLeftRect = UIRect();
            leaf.ScrollRightRect = UIRect();
            leaf.ActiveTabCloseRect = UIRect();
            const UIRect& strip = leaf.TabStripRect;
            if (leaf.Tabs.empty() || strip.height <= 1.0f)
            {
                leaf.TabScrollOffset = 0.0f;
                return;
            }

            const float font = style.FontSize * scale;
            const float pad = style.TabPaddingX * scale;
            const float gap = style.TabGap * scale;
            const float close_w = CloseButtonWidth(font);

            // Pass 1: measure every tab's natural width (the active tab carries the close button).
            std::vector<float> tab_w(leaf.Tabs.size(), 0.0f);
            float total = 0.0f;
            for (size_t i = 0; i < leaf.Tabs.size(); ++i)
            {
                const float text_w = renderer.MeasureText(leaf.Tabs[i].PanelId, font, TextWrapMode::NoWrap, 0.0f, nullptr).x;
                tab_w[i] = text_w + pad * 2.0f + (static_cast<int>(i) == leaf.ActiveTab ? close_w : 0.0f);
                total += tab_w[i] + (i > 0 ? gap : 0.0f);
            }

            const float arrow_w = strip.height;  // square arrow buttons
            const bool overflow = total > strip.width + 0.5f;
            const float usable = overflow ? std::max(1.0f, strip.width - 2.0f * arrow_w) : strip.width;

            // Clamp the persisted scroll to the valid range for the current width.
            const float max_scroll = std::max(0.0f, total - usable);
            if (leaf.TabScrollOffset < 0.0f)
                leaf.TabScrollOffset = 0.0f;
            if (leaf.TabScrollOffset > max_scroll)
                leaf.TabScrollOffset = max_scroll;

            // Pass 2: place tabs (scrolled). Tabs may run under the arrow zone / off-strip;
            // the renderer clips to the usable region so they never paint over the arrows.
            float x = strip.x - leaf.TabScrollOffset;
            for (size_t i = 0; i < leaf.Tabs.size(); ++i)
            {
                leaf.TabRects[i] = UIRect(x, strip.y, tab_w[i], strip.height);
                if (static_cast<int>(i) == leaf.ActiveTab)
                {
                    leaf.ActiveTabCloseRect =
                        UIRect(x + tab_w[i] - close_w, strip.y, close_w, strip.height);
                }
                x += tab_w[i] + gap;
            }

            if (overflow)
            {
                const float ax = strip.x + usable;
                leaf.ScrollLeftRect = UIRect(ax, strip.y, arrow_w, strip.height);
                leaf.ScrollRightRect = UIRect(ax + arrow_w, strip.y, arrow_w, strip.height);
            }
        }
    }  // namespace

    void DockHost::RenderBackgrounds(DockTree& tree, BatchedUIRenderer& renderer)
    {
        tree.ForEachLeaf([&](DockNode& leaf) {
            if (leaf.NodeRect.width <= 1.0f || leaf.NodeRect.height <= 1.0f)
                return;

            // Scene / Game use EditorViewFlags_NoBackground: the swapchain already
            // holds RP2+skybox in the panel work rect; an opaque fill would hide it.
            if (!leaf.Tabs.empty())
            {
                const int active_tab =
                    std::clamp(leaf.ActiveTab, 0, static_cast<int>(leaf.Tabs.size()) - 1);
                const std::string& panel_id = leaf.Tabs[active_tab].PanelId;
                if (panel_id == EditorLayoutWindowIds::kScene || panel_id == EditorLayoutWindowIds::kGame)
                {
                    return;
                }
            }

            // Opaque panel-area fill only. Recorded below the panel content (see
            // RenderBackgrounds doc in DockHost.h); the 1px frame + tab strip stay
            // in Render() at the foreground layer.
            renderer.DrawQuad(leaf.ContentRect, m_Style.PanelBg);
        });
    }

    void DockHost::Render(DockTree& tree, BatchedUIRenderer& renderer, float scale, const Vector2& mouse)
    {
        tree.ForEachNode([&](DockNode& node) {
            if (node.IsLeaf())
                RenderLeaf(node, renderer, scale, mouse);
            else
                RenderSplitter(node, renderer, mouse);
        });
    }

    void DockHost::LayoutTabs(DockTree& tree, BatchedUIRenderer& renderer, float scale)
    {
        tree.ForEachLeaf([&](DockNode& leaf) { LayoutLeafTabs(leaf, renderer, scale, m_Style); });
    }

    void DockHost::DrawDropPreview(BatchedUIRenderer& renderer, const DockNode& target, EDockDir dir) const
    {
        const UIRect& r = target.NodeRect;
        if (r.width <= 1.0f || r.height <= 1.0f)
            return;

        UIRect region = r;
        switch (dir)
        {
            case EDockDir::Left:   region = UIRect(r.x, r.y, r.width * 0.5f, r.height); break;
            case EDockDir::Right:  region = UIRect(r.x + r.width * 0.5f, r.y, r.width * 0.5f, r.height); break;
            case EDockDir::Top:    region = UIRect(r.x, r.y, r.width, r.height * 0.5f); break;
            case EDockDir::Bottom: region = UIRect(r.x, r.y + r.height * 0.5f, r.width, r.height * 0.5f); break;
            case EDockDir::Center:
            default:               region = target.ContentRect; break;
        }
        renderer.DrawQuad(region, m_Style.DropPreviewFill);
        renderer.DrawRect(region, m_Style.DropPreviewBorder, 2.0f);
    }

    void DockHost::RenderLeaf(DockNode& leaf, BatchedUIRenderer& renderer, float scale, const Vector2& mouse)
    {
        const UIRect& node_rect = leaf.NodeRect;
        if (node_rect.width <= 1.0f || node_rect.height <= 1.0f)
            return;

        // 1px frame around the leaf. The opaque panel-area fill is drawn separately by
        // RenderBackgrounds() in a z-group below the panel content (see DockHost.h).
        renderer.DrawRect(node_rect, m_Style.BorderColor, 1.0f);

        if (leaf.Tabs.empty() || leaf.TabStripRect.height <= 1.0f)
        {
            leaf.TabRects.clear();
            return;
        }

        // Tab strip band.
        const UIRect& strip = leaf.TabStripRect;
        renderer.DrawQuad(strip, m_Style.TabStripBg);

        // Tab rects come from the shared layout helper (also used by the input layer so
        // hit-testing matches pixel-for-pixel).
        LayoutLeafTabs(leaf, renderer, scale, m_Style);

        const float font = m_Style.FontSize * scale;
        const float accent_h = std::max(1.0f, 2.0f * scale);
        const float close_w = CloseButtonWidth(font);
        const bool overflow = leaf.ScrollLeftRect.width > 0.0f;
        const float usable = UsableTabWidth(leaf, strip.width, strip.height);

        // Clip tab content to the visible (non-arrow) region so scrolled labels never
        // bleed into siblings or paint over the scroll arrows.
        renderer.PushClipRect(UIRect(strip.x, strip.y, usable, strip.height), true);

        for (size_t i = 0; i < leaf.TabRects.size(); ++i)
        {
            const UIRect& tab_rect = leaf.TabRects[i];
            if (tab_rect.width <= 0.0f)
                continue;

            const std::string& label = leaf.Tabs[i].PanelId;
            const bool is_active = (static_cast<int>(i) == leaf.ActiveTab);
            const bool is_hover = RectContains(tab_rect, mouse);

            ZSlate::UIColor bg = m_Style.TabInactiveBg;
            if (is_active)
                bg = m_Style.TabActiveBg;
            else if (is_hover)
                bg = m_Style.TabHoverBg;
            renderer.DrawQuad(tab_rect, bg);

            if (is_active)
            {
                // Accent bar along the top edge of the active tab (Unity/UE feel).
                renderer.DrawQuad(UIRect(tab_rect.x, tab_rect.y, tab_rect.width, accent_h), m_Style.ActiveTabAccent);
            }

            // Label area excludes the close button on the active tab.
            const float label_w = is_active ? std::max(1.0f, tab_rect.width - close_w) : tab_rect.width;
            renderer.DrawText(UIRect(tab_rect.x, tab_rect.y, label_w, tab_rect.height),
                              label,
                              font,
                              is_active ? m_Style.TabActiveText : m_Style.TabText,
                              TextAnchor::MiddleCenter,
                              TextWrapMode::NoWrap,
                              nullptr);

            if (is_active && leaf.ActiveTabCloseRect.width > 0.0f)
            {
                const bool close_hover = RectContains(leaf.ActiveTabCloseRect, mouse);
                if (close_hover)
                    renderer.DrawQuad(leaf.ActiveTabCloseRect, m_Style.TabHoverBg);
                renderer.DrawText(leaf.ActiveTabCloseRect,
                                  "x",
                                  font,
                                  close_hover ? m_Style.TabActiveText : m_Style.TabText,
                                  TextAnchor::MiddleCenter,
                                  TextWrapMode::NoWrap,
                                  nullptr);
            }
        }

        renderer.PopClipRect();

        // Scroll arrows (drawn over their own background so any tab bleeding into the
        // arrow zone is masked).
        if (overflow)
        {
            const auto draw_arrow = [&](const UIRect& r, const char* glyph) {
                const bool hover = RectContains(r, mouse);
                renderer.DrawQuad(r, hover ? m_Style.TabHoverBg : m_Style.TabInactiveBg);
                renderer.DrawText(r, glyph, font, m_Style.TabActiveText, TextAnchor::MiddleCenter,
                                  TextWrapMode::NoWrap, nullptr);
            };
            draw_arrow(leaf.ScrollLeftRect, "<");
            draw_arrow(leaf.ScrollRightRect, ">");
        }
    }

    void DockHost::RenderSplitter(DockNode& split, BatchedUIRenderer& renderer, const Vector2& mouse)
    {
        const UIRect& r = split.SplitterRect;
        if (r.width <= 0.0f || r.height <= 0.0f)
            return;

        const bool hover = RectContains(r, mouse);
        renderer.DrawQuad(r, hover ? m_Style.SplitterHoverColor : m_Style.SplitterColor);
    }
}  // namespace EditorDock
