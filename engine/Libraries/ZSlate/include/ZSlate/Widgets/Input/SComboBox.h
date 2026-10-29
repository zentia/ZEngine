#pragma once

#include "ZSlate/Application/SlateInput.h"
#include "ZSlate/Widgets/SCompoundWidget.h"
#include "ZSlate/Widgets/Input/SButton.h"
#include "ZSlate/Widgets/Text/STextBlock.h"
#include "ZSlate/Widgets/SPanel.h"
#include "ZSlate/Widgets/Layout/SScrollBox.h"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace ZSlate
{

// =============================================================================
// SComboBox — dropdown selection (UE SComboBox analogue)
// =============================================================================
//
// Displays the selected item; clicking opens a dropdown list.
//
// The default label for std::string items is the string itself, so a
// SComboBox<std::string> works without an OnGetLabel callback.
template <typename T>
inline std::string ComboBoxDefaultLabel(const T&, int32_t index) { return "#" + std::to_string(index); }
inline std::string ComboBoxDefaultLabel(const std::string& item, int32_t) { return item; }

//
// While open, the dropdown is registered with the SlateInputRouter popup
// layer: SlateApplication::PaintInto draws it above everything else and the
// router feeds it all mouse input until it closes. Hosts with their own
// overlay can set ExternalPopup and drive PaintPopup / ProcessPopupMouse.
template <typename T = std::string>
class SComboBox : public SCompoundWidget, public ISlatePopup
{
public:
    // Data
    std::vector<T> Options;
    T SelectedItem {};
    int32_t SelectedIndex {-1};

    // Styling
    float ItemHeight {22.0f};
    float FontSize {13.0f};
    float MaxDropdownHeight {200.0f};
    Vector2 Size {160.0f, 26.0f};
    // When > 0 overrides Size.y (handy when the width is left to the parent).
    float Height {0.0f};
    // Shown while nothing is selected.
    std::string Placeholder {"..."};
    // Overlay hosts call PaintPopup after the root, and ProcessPopupMouse before routing to the root.
    bool ExternalPopup {false};

    // Callbacks
    std::function<std::string(const T&)> OnGetLabel;
    std::function<void(const T&, int32_t)> OnSelectionChanged;

    void SetOptions(std::vector<T> options)
    {
        Options = std::move(options);
        ClosePopup();
        if (SelectedIndex >= static_cast<int32_t>(Options.size()))
        {
            SelectedIndex = -1;
            SelectedItem = T {};
        }
    }

    void SetSelectedIndex(int32_t index)
    {
        if (index < 0 || index >= static_cast<int32_t>(Options.size()))
        {
            SelectedIndex = -1;
            SelectedItem = T {};
            return;
        }
        SelectedIndex = index;
        SelectedItem = Options[index];
    }

    Vector2 ComputeDesiredSize() const override { return Vector2(Size.x, Height > 0.0f ? Height : Size.y); }
    bool IsOpen() const { return m_Open; }
    bool IsPopupOpen() const override { return m_Open; }
    void ClosePopup()
    {
        m_Open = false;
        m_PressedItem = -1;
        SlateInputRouter::ClosePopup(this);
    }
    bool SupportsKeyboardFocus() const override { return true; }
    void OnFocusLost() override { ClosePopup(); }
    void OnMouseCaptureLost() override { ClosePopup(); }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (!ctx.Renderer) return;
        const UIRect r = geom.ToRect();

        // Closed state: just the selected value as a button-like box
        UIColor bg = m_Open ? UIColor(0.20f, 0.22f, 0.28f, 1.0f)
                   : m_Hovered ? UIColor(0.16f, 0.17f, 0.21f, 1.0f)
                   : UIColor(0.12f, 0.12f, 0.14f, 1.0f);
        ctx.Renderer->DrawQuad(r, bg);
        ctx.Renderer->DrawRect(r, UIColor(0.35f, 0.38f, 0.45f, 1.0f), 1.0f);

        // Arrow
        const char* arrow = m_Open ? "\xE2\x96\xB2" : "\xE2\x96\xBC";  // ▲ or ▼
        ctx.Renderer->DrawTextLabel(
            UIRect(r.Right() - 24.0f, r.y, 20.0f, r.h),
            arrow, FontSize, UIColor(0.7f, 0.7f, 0.7f, 1.0f),
            TextAnchor::MiddleCenter, TextWrapMode::NoWrap);

        // Selected text
        const bool hasSelection = SelectedIndex >= 0 && SelectedIndex < static_cast<int32_t>(Options.size());
        const std::string label = hasSelection ? GetLabel(SelectedIndex) : Placeholder;

        ctx.Renderer->DrawTextLabel(
            UIRect(r.x + 6.0f, r.y, r.w - 30.0f, r.h),
            label, FontSize,
            hasSelection ? UIColor(0.88f, 0.89f, 0.92f, 1.0f) : UIColor(0.50f, 0.52f, 0.58f, 1.0f),
            TextAnchor::MiddleLeft, TextWrapMode::NoWrap);
        // The dropdown itself is painted through the popup layer (PaintPopup).
    }

    // ---- Input ---------------------------------------------------------------

    void OnMouseEnter() override { m_Hovered = true; }
    void OnMouseLeave() override { m_Hovered = false; }

    FReply OnMouseButtonDown(const Vector2& pos, int btn) override
    {
        if (btn != 0) return FReply::Unhandled();
        if (m_Open) ProcessPopupMouse(pos, true, 0);
        else OpenPopup(true);
        return FReply::Handled();
    }

    FReply OnMouseButtonUp(const Vector2& pos, int btn) override
    {
        if (btn != 0) return FReply::Unhandled();
        ProcessPopupMouse(pos, false, 0);
        return FReply::Handled();
    }

    FReply OnKeyDown(EKey key) override
    {
        if (Options.empty()) return FReply::Unhandled();
        if (key == EKey::Escape || key == EKey::Tab)
        {
            ClosePopup();
            return FReply::Handled();
        }
        if (key != EKey::Enter && key != EKey::Space && key != EKey::Up && key != EKey::Down)
            return FReply::Unhandled();
        if (!m_Open) OpenPopup(false);
        else if (key == EKey::Enter || key == EKey::Space) Commit(m_ActiveItem);
        else
        {
            m_ActiveItem = std::clamp(m_ActiveItem + (key == EKey::Down ? 1 : -1), 0, static_cast<int32_t>(Options.size()) - 1);
            EnsureActiveVisible();
        }
        return FReply::Handled();
    }

    // Returns true even for a dismissing click: the host must not click through to the root.
    bool ProcessPopupMouse(const Vector2& pos, bool leftDown, float wheel) override
    {
        if (!m_Open) return false;
        const int32_t hit = HitItem(pos);
        if (hit >= 0) m_ActiveItem = hit;
        if (wheel != 0 && m_VisibleRows > 0)
        {
            m_FirstItem = std::clamp(m_FirstItem + (wheel > 0 ? -1 : 1), 0,
                std::max(0, static_cast<int32_t>(Options.size()) - m_VisibleRows));
        }
        if (leftDown && !m_PopupLeftDown)
        {
            m_PressedItem = hit;
            if (hit < 0) ClosePopup();
        }
        else if (!leftDown && m_PopupLeftDown && m_PressedItem >= 0)
        {
            if (hit == m_PressedItem) Commit(hit);
            else m_PressedItem = -1;
        }
        m_PopupLeftDown = leftDown;
        return true;
    }

    void PaintPopup(ISlateRenderer* renderer, const UIRect& viewport) const override
    {
        if (!m_Open || !renderer) return;
        UpdatePopupRect(viewport);
        FPaintContext context;
        context.Renderer = renderer;
        renderer->PushClipRect(viewport);
        PaintDropdown(context);
        renderer->PopClipRect();
    }

private:
    bool m_Hovered {false};
    bool m_Open {false};
    bool m_PopupLeftDown {false};
    int32_t m_PressedItem {-1};
    int32_t m_ActiveItem {-1};
    mutable int32_t m_FirstItem {0};
    mutable int32_t m_VisibleRows {0};
    mutable UIRect m_PopupRect;

    std::string GetLabel(int32_t index) const
    {
        return OnGetLabel ? OnGetLabel(Options[index]) : ComboBoxDefaultLabel(Options[index], index);
    }

    void OpenPopup(bool leftDown)
    {
        if (Options.empty() || ItemHeight <= 0) return;
        m_Open = true;
        m_PopupLeftDown = leftDown;
        m_PressedItem = -1;
        m_ActiveItem = std::clamp(SelectedIndex, 0, static_cast<int32_t>(Options.size()) - 1);
        m_FirstItem = 0;
        if (!ExternalPopup)
            SlateInputRouter::OpenPopup(std::const_pointer_cast<SWidget>(shared_from_this()));
    }
    void Commit(int32_t index)
    {
        ClosePopup();
        if (index < 0 || index >= static_cast<int32_t>(Options.size())) return;
        const bool changed = SelectedIndex != index;
        SelectedIndex = index;
        SelectedItem = Options[index];
        if (changed && OnSelectionChanged) OnSelectionChanged(SelectedItem, index);
    }
    void EnsureActiveVisible() const
    {
        if (m_VisibleRows < 1) return;
        if (m_ActiveItem < m_FirstItem) m_FirstItem = m_ActiveItem;
        if (m_ActiveItem >= m_FirstItem + m_VisibleRows) m_FirstItem = m_ActiveItem - m_VisibleRows + 1;
    }
    void UpdatePopupRect(const UIRect& viewport) const
    {
        const UIRect anchor = GetCachedGeometry().ToRect();
        const float below = std::max(0.0f, viewport.Bottom() - anchor.Bottom());
        const float above = std::max(0.0f, anchor.y - viewport.y);
        const float desired = std::min(MaxDropdownHeight, ItemHeight * static_cast<float>(Options.size()));
        const bool flip = desired > below && above > below;
        const float available = std::min(desired, flip ? above : below);
        m_VisibleRows = ItemHeight > 0 ? std::max(0, static_cast<int32_t>(available / ItemHeight)) : 0;
        const float width = std::min(anchor.w, viewport.w);
        m_PopupRect = UIRect(std::clamp(anchor.x, viewport.x, viewport.Right() - width),
            flip ? anchor.y - m_VisibleRows * ItemHeight : anchor.Bottom(), width, m_VisibleRows * ItemHeight);
        m_FirstItem = std::clamp(m_FirstItem, 0, std::max(0, static_cast<int32_t>(Options.size()) - m_VisibleRows));
    }
    int32_t HitItem(const Vector2& pos) const
    {
        if (ItemHeight <= 0 || pos.x < m_PopupRect.x || pos.x >= m_PopupRect.Right() ||
            pos.y < m_PopupRect.y || pos.y >= m_PopupRect.Bottom()) return -1;
        const int32_t index = m_FirstItem + static_cast<int32_t>((pos.y - m_PopupRect.y) / ItemHeight);
        return index < static_cast<int32_t>(Options.size()) ? index : -1;
    }
    void PaintDropdown(const FPaintContext& ctx) const
    {
        if (m_VisibleRows < 1) return;
        ctx.Renderer->DrawQuad(m_PopupRect, UIColor(0.14f, 0.15f, 0.18f, 1.0f));
        ctx.Renderer->DrawRect(m_PopupRect, UIColor(0.35f, 0.38f, 0.45f, 1.0f), 1.0f);
        ctx.Renderer->PushClipRect(m_PopupRect);
        for (int32_t row = 0; row < m_VisibleRows; ++row)
        {
            const int32_t i = m_FirstItem + row;
            UIRect item(m_PopupRect.x + 1, m_PopupRect.y + row * ItemHeight, m_PopupRect.w - 2, ItemHeight);
            if (i == m_ActiveItem)
                ctx.Renderer->DrawQuad(item, UIColor(0.18f, 0.35f, 0.62f, 1.0f));
            const std::string label = GetLabel(i);
            ctx.Renderer->DrawTextLabel(UIRect(item.x + 6, item.y, item.w - 12, item.h), label, FontSize,
                UIColor(0.95f, 0.95f, 0.95f, 1.0f), TextAnchor::MiddleLeft, TextWrapMode::NoWrap);
        }
        ctx.Renderer->PopClipRect();
    }
};

}  // namespace ZSlate
