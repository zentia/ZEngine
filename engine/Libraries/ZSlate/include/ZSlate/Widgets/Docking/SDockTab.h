#pragma once

#include "ZSlate/Widgets/SCompoundWidget.h"

#include <functional>
#include <memory>
#include <string>

namespace ZSlate
{

// =============================================================================
// SDockTab — dockable tab widget (UE SDockTab analogue)
// =============================================================================
//
// A single tab in a dock area. Has a label, a close button, and content.
// Activated/deactivated states drive visual changes.
//
class SDockTab : public SCompoundWidget
{
public:
    std::string Label {"Tab"};
    bool CanClose {true};

    // Callbacks
    std::function<void()> OnTabClosed;
    std::function<void()> OnTabActivated;

    // Active state (set by SDockingArea when the user clicks this tab)
    bool IsActive() const { return m_Active; }
    void SetActive(bool active) { m_Active = active; }

    Vector2 ComputeDesiredSize() const override
    {
        // Tab button size (label text width)
        float w = static_cast<float>(Label.size()) * FontSize * 0.55f + 48.0f;  // text + close btn + padding
        return Vector2(w, FontSize + 8.0f);
    }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (!ctx.Renderer) return;
        const UIRect r = geom.ToRect();

        // Tab background
        UIColor bg = m_Active
            ? UIColor(0.18f, 0.19f, 0.22f, 1.0f)
            : (m_Hovered
                ? UIColor(0.14f, 0.15f, 0.18f, 1.0f)
                : UIColor(0.10f, 0.10f, 0.13f, 1.0f));

        ctx.Renderer->DrawQuad(r, bg);

        // Active indicator (top bar)
        if (m_Active)
            ctx.Renderer->DrawQuad(
                UIRect(r.x, r.y, r.w, 3.0f),
                UIColor(0.30f, 0.55f, 0.95f, 1.0f));

        // Label
        float textX = r.x + 10.0f;
        float closeBtnW = CanClose ? 20.0f : 0.0f;
        ctx.Renderer->DrawTextLabel(
            UIRect(textX, r.y, r.w - 10.0f - closeBtnW, r.h),
            Label, FontSize,
            m_Active ? UIColor(0.95f, 0.95f, 1.0f, 1.0f)
                     : UIColor(0.75f, 0.77f, 0.82f, 1.0f),
            TextAnchor::MiddleLeft, TextWrapMode::NoWrap);

        // Close button
        if (CanClose)
        {
            float cx = r.Right() - 18.0f;
            ctx.Renderer->DrawTextLabel(
                UIRect(cx, r.y, 14.0f, r.h),
                "\xC3\x97", FontSize + 1.0f,  // ×
                m_CloseHovered ? UIColor(1.0f, 0.3f, 0.3f, 1.0f)
                               : UIColor(0.5f, 0.5f, 0.5f, 1.0f),
                TextAnchor::MiddleCenter, TextWrapMode::NoWrap);
        }
    }

    // ---- Input ---------------------------------------------------------------

    void OnMouseEnter() override { m_Hovered = true; }
    void OnMouseLeave() override { m_Hovered = false; m_CloseHovered = false; }

    void OnMouseMove(const Vector2& pos) override
    {
        const UIRect r = GetCachedGeometry().ToRect();
        m_CloseHovered = CanClose && pos.x > r.Right() - 18.0f;
    }

    FReply OnMouseButtonDown(const Vector2& pos, int btn) override
    {
        if (btn != 0) return FReply::Unhandled();

        const UIRect r = GetCachedGeometry().ToRect();
        if (CanClose && pos.x > r.Right() - 18.0f)
        {
            if (OnTabClosed) OnTabClosed();
            return FReply::Handled();
        }

        m_Active = true;
        if (OnTabActivated) OnTabActivated();
        return FReply::Handled();
    }

private:
    mutable bool m_Active {false};
    mutable bool m_Hovered {false};
    mutable bool m_CloseHovered {false};
    float FontSize {12.0f};
};

// =============================================================================
// SDockingArea — container for dockable tabs (UE SDockingArea analogue)
// =============================================================================
//
// Holds a row of tab headers and the active tab's content.
//
class SDockingArea : public SWidget
{
public:
    void AddTab(std::shared_ptr<SDockTab> tab)
    {
        if (!tab) return;
        tab->OnTabActivated = [this, tab]()
        {
            m_ActiveTab = tab;
            // Deactivate others
            for (auto& t : m_Tabs)
                if (t != tab) t->SetActive(false);
        };
        m_Tabs.push_back(tab);
        if (!m_ActiveTab)
        {
            m_ActiveTab = tab;
            tab->SetActive(true);
        }
    }

    void RemoveTab(std::shared_ptr<SDockTab> tab)
    {
        auto it = std::find(m_Tabs.begin(), m_Tabs.end(), tab);
        if (it != m_Tabs.end())
        {
            if (m_ActiveTab == tab)
            {
                m_ActiveTab = nullptr;
                // Activate neighbor
                auto idx = static_cast<int32_t>(it - m_Tabs.begin());
                if (idx + 1 < static_cast<int32_t>(m_Tabs.size()))
                {
                    m_ActiveTab = m_Tabs[idx + 1];
                    m_ActiveTab->SetActive(true);
                }
                else if (idx > 0)
                {
                    m_ActiveTab = m_Tabs[idx - 1];
                    m_ActiveTab->SetActive(true);
                }
            }
            m_Tabs.erase(it);
        }
    }

    int32_t GetTabCount() const { return static_cast<int32_t>(m_Tabs.size()); }

    Vector2 ComputeDesiredSize() const override
    {
        // Tab bar height + active content height
        float tabBarH = 28.0f;
        float contentH = 0.0f;
        if (m_ActiveTab)
        {
            // Content is whatever the tab's child would be
            auto content = m_ActiveTab->GetContent();
            if (content) contentH = content->GetDesiredSize().y;
        }
        return Vector2(400.0f, tabBarH + contentH);
    }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (!ctx.Renderer) return;
        const UIRect r = geom.ToRect();

        // Tab bar background
        float tabBarH = 28.0f;
        ctx.Renderer->DrawQuad(UIRect(r.x, r.y, r.w, tabBarH),
                               UIColor(0.06f, 0.06f, 0.08f, 1.0f));

        // Layout tabs horizontally
        float cursorX = r.x;
        for (const auto& tab : m_Tabs)
        {
            Vector2 tabSize = tab->GetDesiredSize();
            FGeometry tabGeom = geom.MakeChild(
                Vector2(cursorX - r.x, 0), tabSize);
            tab->Paint(ctx, tabGeom);
            cursorX += tabSize.x;
        }

        // Content area
        if (m_ActiveTab)
        {
            float contentY = r.y + tabBarH;
            UIRect contentRect(r.x, contentY, r.w, r.h - tabBarH);

            ctx.Renderer->DrawQuad(contentRect,
                UIColor(0.08f, 0.08f, 0.10f, 1.0f));

            auto content = m_ActiveTab->GetContent();
            if (content)
            {
                FGeometry contentGeom = geom.MakeChild(
                    Vector2(0, tabBarH), Vector2(r.w, r.h - tabBarH));
                content->Paint(ctx, contentGeom);
            }
        }
    }

    // Input
    FReply OnMouseButtonDown(const Vector2& pos, int btn) override
    {
        if (btn != 0) return FReply::Unhandled();
        const UIRect r = GetCachedGeometry().ToRect();
        float tabBarH = 28.0f;

        // Only handle tabs area (top 28px)
        if (pos.y < r.y || pos.y > r.y + tabBarH) return FReply::Unhandled();

        float cursorX = r.x;
        for (auto& tab : m_Tabs)
        {
            float tabW = tab->GetDesiredSize().x;
            if (pos.x >= cursorX && pos.x <= cursorX + tabW)
            {
                // Activate this tab
                for (auto& t : m_Tabs)
                    t->SetActive(t == tab);
                m_ActiveTab = tab;
                if (tab->OnTabActivated) tab->OnTabActivated();
                return FReply::Handled();
            }
            cursorX += tabW;
        }
        return FReply::Unhandled();
    }

private:
    std::vector<std::shared_ptr<SDockTab>> m_Tabs;
    std::shared_ptr<SDockTab> m_ActiveTab;
};

}  // namespace ZSlate
