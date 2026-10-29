#pragma once

#include "ZSlate/Widgets/Panels/SBorder.h"
#include "ZSlate/Widgets/Layout/SBoxPanel.h"
#include "ZSlate/Widgets/SCompoundWidget.h"
#include "ZSlate/Widgets/Text/STextBlock.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace ZSlate
{
// One selectable row in an SMenu (UE Slate SMenuEntryBlock analogue). Full-width,
// left-aligned label, hover highlight, fires its action on left-click release.
//
// Optional decorations drawn in the reserved right gutter:
//   - ShowCheck + Checked : a small filled square marker (the ImGui default font
//     atlas has no U+2713 glyph, so a drawn square is used instead).
//   - HasSubMenu          : an ASCII '>' arrow; the SMenu host opens the nested
//     submenu on hover (no action fired, no close requested).
//   - Disabled            : dimmed label, no hover highlight, no click.
class SMenuItem : public SCompoundWidget
{
public:
    UIColor HoverColor {0.26f, 0.40f, 0.62f, 1.0f};
    UIColor MarkColor {0.90f, 0.91f, 0.94f, 1.0f};
    std::function<void()> OnClicked;

    float FontSize {14.0f};
    bool ShowCheck {false};
    bool Checked {false};
    bool HasSubMenu {false};
    bool Disabled {false};

    SMenuItem()
    {
        HAlign = EHorizontalAlignment::Fill;
        VAlign = EVerticalAlignment::Center;
    }

    // Width reserved on the right for the check / submenu marker.
    float MarkerGutter() const { return (ShowCheck || HasSubMenu) ? FontSize + 6.0f : 0.0f; }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (ctx.Renderer == nullptr)
            return;

        const UIRect rect = geom.ToRect();
        if (m_Hovered && !Disabled)
            ctx.Renderer->DrawQuad(rect, HoverColor);

        const float gutter = MarkerGutter();
        if (gutter <= 0.0f)
            return;

        const UIColor mark = Disabled ? UIColor(MarkColor.x, MarkColor.y, MarkColor.z, 0.4f) : MarkColor;
        if (HasSubMenu)
        {
            // ZSlate::UIRect uses .w/.h (not .width/.height)
            ctx.Renderer->DrawTextLabel(UIRect(rect.x + rect.w - gutter, rect.y, gutter, rect.h),
                                   ">", FontSize, mark, TextAnchor::MiddleCenter, TextWrapMode::NoWrap, nullptr);
        }
        else if (ShowCheck)
        {
            const float s = FontSize * 0.5f;
            const float mx = rect.x + rect.w - gutter + (gutter - s) * 0.5f;
            const float my = rect.y + (rect.h - s) * 0.5f;
            if (Checked)
            {
                ctx.Renderer->DrawQuad(UIRect(mx, my, s, s), mark);
            }
            else
            {
                // Hollow box so off-state is visible (font has no check glyph).
                const float t = std::max(1.0f, s * 0.18f);
                ctx.Renderer->DrawQuad(UIRect(mx, my, s, t), mark);
                ctx.Renderer->DrawQuad(UIRect(mx, my + s - t, s, t), mark);
                ctx.Renderer->DrawQuad(UIRect(mx, my, t, s), mark);
                ctx.Renderer->DrawQuad(UIRect(mx + s - t, my, t, s), mark);
            }
        }
    }

    Vector2 ComputeDesiredSize() const override
    {
        Vector2 size = SCompoundWidget::ComputeDesiredSize();
        size.x += MarkerGutter();
        return size;
    }

    void OnMouseEnter() override { m_Hovered = true; }
    void OnMouseLeave() override { m_Hovered = false; }
    void OnMouseCaptureLost() override { m_Pressed = false; }

    FReply OnMouseButtonDown(const Vector2& /*pos*/, int button) override
    {
        if (button != 0 || Disabled || HasSubMenu)
            return FReply::Unhandled();
        m_Pressed = true;
        return FReply::Handled().CaptureMouse(this);
    }

    FReply OnMouseButtonUp(const Vector2& screen_pos, int button) override
    {
        if (button != 0)
            return FReply::Unhandled();
        const bool was_pressed = m_Pressed;
        m_Pressed = false;
        if (was_pressed && !Disabled && m_CachedGeometry.IsUnderLocation(screen_pos) && OnClicked)
            OnClicked();
        return FReply::Handled().ReleaseMouseCapture();
    }

    bool m_Hovered {false};
    bool m_Pressed {false};
};

// A floating list of menu items (UE Slate SMenu analogue). Built by the owner
// (usually a window) at the cursor position and painted/routed on top of the
// main tree. Picking any item sets the close-requested flag so the host can
// dismiss the menu after routing the click.
//
// Hosting model: SMenu does NOT paint or route itself. The window that opened it
// paints it (`Paint`) at a clamped position and feeds it a second input pass,
// then checks IsCloseRequested(). This keeps the popup per-window with no global
// state -- a context menu opened in the Hierarchy lives and dies in the Hierarchy.
class SMenu : public SCompoundWidget
{
public:
    float MinWidth {180.0f};
    UIColor BackgroundColor {0.16f, 0.16f, 0.18f, 0.98f};

    SMenu()
    {
        m_List = std::make_shared<SVerticalBox>();

        auto border = std::make_shared<SBorder>();
        border->BackgroundColor = BackgroundColor;
        border->Padding = FMargin(2.0f, 2.0f);
        border->SetContent(m_List);
        SetContent(border);
        m_Border = border;
    }

    void Clear()
    {
        m_List->ClearChildren();
        m_SubMenus.clear();
        m_CloseRequested = false;
    }

    // `scale` lets the host match the editor's DPI scale (font + padding).
    std::shared_ptr<SMenuItem> AddItem(const std::string& label,
                                       std::function<void()> action,
                                       float scale = 1.0f,
                                       bool disabled = false)
    {
        auto item = MakeRow(label, scale);
        item->Disabled = disabled;
        item->OnClicked = [this, action = std::move(action)]() {
            if (action)
                action();
            m_CloseRequested = true;
        };
        m_List->AddSlot(item).AutoSize().SetHAlign(EHorizontalAlignment::Fill);
        return item;
    }

    // A row with a right-aligned check marker (toggle item).
    std::shared_ptr<SMenuItem> AddCheckItem(const std::string& label,
                                            bool checked,
                                            std::function<void()> action,
                                            float scale = 1.0f)
    {
        auto item = MakeRow(label, scale);
        item->ShowCheck = true;
        item->Checked = checked;
        item->OnClicked = [this, action = std::move(action)]() {
            if (action)
                action();
            m_CloseRequested = true;
        };
        m_List->AddSlot(item).AutoSize().SetHAlign(EHorizontalAlignment::Fill);
        return item;
    }

    // A row that opens a nested submenu (drawn/routed by the host menu stack).
    // Returns the child SMenu to populate. Clicking the row does NOT close.
    std::shared_ptr<SMenu> AddSubMenu(const std::string& label, float scale = 1.0f)
    {
        auto item = MakeRow(label, scale);
        item->HasSubMenu = true;
        m_List->AddSlot(item).AutoSize().SetHAlign(EHorizontalAlignment::Fill);

        auto child = std::make_shared<SMenu>();
        m_SubMenus.push_back({item, child});
        return child;
    }

    // The child submenu whose row is currently hovered, or nullptr. Used by the
    // host menu stack to decide which nested popup to open.
    std::shared_ptr<SMenu> GetHoveredSubMenu() const
    {
        for (const auto& entry : m_SubMenus)
        {
            if (entry.first && entry.first->m_Hovered)
                return entry.second;
        }
        return nullptr;
    }

    // Screen geometry of the submenu row owning `child` (anchor for the popup).
    FGeometry GetSubMenuAnchor(const std::shared_ptr<SMenu>& child) const
    {
        for (const auto& entry : m_SubMenus)
        {
            if (entry.second == child && entry.first)
                return entry.first->GetCachedGeometry();
        }
        return FGeometry();
    }

    // True if the cursor is over any row of this menu (host uses this to keep a
    // parent open while a child submenu is being navigated).
    bool IsAnyRowHovered() const
    {
        for (const auto& entry : m_SubMenus)
        {
            if (entry.first && entry.first->m_Hovered)
                return true;
        }
        return false;
    }

    void AddSeparator(float scale = 1.0f)
    {
        auto line = std::make_shared<SBorder>();
        line->BackgroundColor = UIColor(0.30f, 0.30f, 0.33f, 1.0f);
        line->Padding = FMargin(0.0f, std::max(0.5f, scale * 0.5f));
        m_List->AddSlot(line)
            .AutoSize()
            .SetHAlign(EHorizontalAlignment::Fill)
            .SetPadding(FMargin(4.0f * scale, 3.0f * scale));
    }

    Vector2 ComputeDesiredSize() const override
    {
        if (m_Border)
            m_Border->BackgroundColor = BackgroundColor;
        Vector2 size = SCompoundWidget::ComputeDesiredSize();
        size.x = std::max(size.x, MinWidth);
        return size;
    }

    bool IsCloseRequested() const { return m_CloseRequested; }
    void RequestClose() { m_CloseRequested = true; }
    bool IsEmpty() const { return m_List->GetChildCount() == 0; }

private:
    std::shared_ptr<SMenuItem> MakeRow(const std::string& label, float scale)
    {
        auto text = std::make_shared<STextBlock>();
        text->Text = label;
        text->FontSize = 14.0f * scale;
        text->Color = UIColor(0.90f, 0.90f, 0.92f, 1.0f);
        text->Alignment = TextAnchor::MiddleLeft;

        auto item = std::make_shared<SMenuItem>();
        item->FontSize = 14.0f * scale;
        item->Padding = FMargin(10.0f * scale, 5.0f * scale);
        item->SetContent(text);
        return item;
    }

    std::shared_ptr<SVerticalBox> m_List;
    std::shared_ptr<SBorder> m_Border;
    std::vector<std::pair<std::shared_ptr<SMenuItem>, std::shared_ptr<SMenu>>> m_SubMenus;
    bool m_CloseRequested {false};
};
}  // namespace ZSlate
