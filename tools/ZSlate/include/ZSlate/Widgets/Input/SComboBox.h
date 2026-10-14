#pragma once

// ============================================================================
// ZSlate/Widgets/Input/SComboBox.h
//
// Drop-down list — the WinForms ComboBox / UE SComboBox analogue.
//
//   auto combo = std::make_shared<ZSlate::SComboBox>();
//   combo->SetOptions({ "HarmonyOS", "Android", "Windows" });
//   combo->SetSelectedIndex(0);
//   combo->OnSelectionChanged = [](int32_t index, const std::string& value) { ... };
//
// Click the box (or press Enter) to open, click an entry to commit, click away
// or press Escape to cancel. The drop-down is painted by the combo itself and
// deliberately overhangs its own geometry, so it can spill over whatever is
// below it.
// ============================================================================

#include "../SWidget.h"
#include "../../Core/ZSlateTypes.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ZSlate
{

class SComboBox : public SWidget
{
public:
    std::vector<std::string> Options;
    int32_t                  SelectedIndex = -1;

    float FontSize          = 14.0f;
    float Height            = 28.0f;
    float ItemHeight        = 24.0f;
    float MaxDropDownHeight = 240.0f;
    float Padding           = 8.0f;

    std::string Placeholder = "(none)";

    UIColor BackgroundColor     { 0.14f, 0.15f, 0.18f, 1.0f };
    UIColor BorderColor         { 0.28f, 0.30f, 0.36f, 1.0f };
    UIColor TextColor           { 0.88f, 0.90f, 0.94f, 1.0f };
    UIColor ArrowColor          { 0.60f, 0.64f, 0.72f, 1.0f };
    UIColor DropDownBackground  { 0.12f, 0.13f, 0.16f, 1.0f };
    UIColor DropDownBorder      { 0.30f, 0.33f, 0.40f, 1.0f };
    UIColor DropDownTextColor   { 0.85f, 0.87f, 0.92f, 1.0f };
    UIColor HighlightColor      { 0.25f, 0.48f, 0.90f, 0.90f };

    std::function<void(int32_t index, const std::string& value)> OnSelectionChanged;

    // ---- Options / selection ----------------------------------------------

    void SetOptions(std::vector<std::string> options)
    {
        Options = std::move(options);
        if (SelectedIndex >= static_cast<int32_t>(Options.size())) SelectedIndex = -1;
        m_HighlightIndex = (SelectedIndex >= 0) ? SelectedIndex
                                                : (Options.empty() ? -1 : 0);
        ClampDropDownScroll();
    }

    void SetSelectedIndex(int32_t index, bool notify = true)
    {
        if (index < 0 || index >= static_cast<int32_t>(Options.size())) index = -1;

        const bool changed = (index != SelectedIndex);
        SelectedIndex = index;
        if (index >= 0) m_HighlightIndex = index;

        if (changed && notify && OnSelectionChanged)
            OnSelectionChanged(SelectedIndex, GetSelectedItem());
    }

    void SetSelectedItem(const std::string& value)
    {
        for (int32_t i = 0; i < static_cast<int32_t>(Options.size()); ++i)
        {
            if (Options[static_cast<size_t>(i)] == value) { SetSelectedIndex(i); return; }
        }
    }

    std::string GetSelectedItem() const
    {
        if (SelectedIndex < 0 || SelectedIndex >= static_cast<int32_t>(Options.size())) return "";
        return Options[static_cast<size_t>(SelectedIndex)];
    }

    bool HasSelection() const { return SelectedIndex >= 0; }

    // ---- Open state --------------------------------------------------------

    bool IsOpen() const { return m_Open; }

    void SetOpen(bool open)
    {
        m_Open = open;
        if (m_Open)
        {
            m_HighlightIndex = (SelectedIndex >= 0) ? SelectedIndex
                                                    : (Options.empty() ? -1 : 0);
            ScrollHighlightIntoView();
        }
    }

    // ---- Widget overrides --------------------------------------------------

    bool WantsFillWidth() const override { return true; }

    Vector2 ComputeDesiredSize() const override { return Vector2(180.0f, Height); }

    bool IsFocusable() const override { return true; }

    void OnFocusLost() override { m_Open = false; }

    void OnPaint(const FPaintContext& context, const FGeometry& geometry) const override
    {
        if (!context.Renderer) return;

        const UIRect box = geometry.ToRect();

        // --- closed state: the box ------------------------------------------
        context.Renderer->DrawQuad(box, BackgroundColor);
        context.Renderer->DrawRect(box, m_Open ? HighlightColor : BorderColor, 1.0f);

        const std::string value = GetSelectedItem();
        const std::string text  = value.empty() ? Placeholder : value;

        context.Renderer->DrawTextLabel(
            UIRect(box.x + Padding, box.y, std::max(0.0f, box.w - 28.0f), box.h),
            text, FontSize,
            value.empty() ? UIColor(0.55f, 0.58f, 0.66f, 1.0f) : TextColor,
            TextAnchor::MiddleLeft, TextWrapMode::NoWrap);

        // Arrow
        context.Renderer->DrawTextLabel(
            UIRect(box.Right() - 20.0f, box.y, 16.0f, box.h),
            m_Open ? "\xE2\x96\xB2" : "\xE2\x96\xBC", FontSize - 3.0f,
            ArrowColor, TextAnchor::MiddleCenter, TextWrapMode::NoWrap);

        if (!m_Open || Options.empty()) return;

        // --- open state: the drop-down --------------------------------------
        const UIRect drop = GetDropDownRect();
        context.Renderer->DrawQuad(drop, DropDownBackground);
        context.Renderer->DrawRect(drop, DropDownBorder, 1.0f);

        const int32_t top     = m_DropTopIndex;
        const int32_t visible = GetVisibleItemCount();

        for (int32_t i = top; i < top + visible && i < static_cast<int32_t>(Options.size()); ++i)
        {
            const UIRect rect = GetItemRect(i);
            if (i == m_HighlightIndex)
                context.Renderer->DrawQuad(rect, HighlightColor);

            context.Renderer->DrawTextLabel(
                UIRect(rect.x + Padding, rect.y, std::max(0.0f, rect.w - Padding * 2.0f), rect.h),
                Options[static_cast<size_t>(i)], FontSize,
                (i == SelectedIndex) ? TextColor : DropDownTextColor,
                TextAnchor::MiddleLeft, TextWrapMode::NoWrap);
        }
    }

    FReply OnMouseButtonDown(const Vector2& absolutePos, int button) override
    {
        if (button != 0) return FReply::Unhandled();

        if (m_Open)
        {
            const int32_t index = HitTestDropDownItem(absolutePos);
            if (index >= 0)
            {
                SetOpen(false);
                SetSelectedIndex(index);
                return FReply::Handled();
            }
            // Clicked the box again (or anywhere else this widget owns): close.
            SetOpen(false);
            return FReply::Handled();
        }

        SetOpen(true);
        return FReply::Handled();
    }

    FReply OnMouseMove(const Vector2& absolutePos) override
    {
        if (!m_Open) return FReply::Unhandled();

        const int32_t index = HitTestDropDownItem(absolutePos);
        if (index >= 0 && index != m_HighlightIndex)
        {
            m_HighlightIndex = index;
            ScrollHighlightIntoView();
        }
        return FReply::Handled();
    }

    FReply OnMouseWheel(const Vector2& absolutePos, float delta) override
    {
        if (!m_Open)
        {
            // Closed: the wheel steps through options, like a spinner.
            if (Options.empty()) return FReply::Unhandled();
            const int32_t count = static_cast<int32_t>(Options.size());
            const int32_t next  = (SelectedIndex < 0) ? 0
                                : std::clamp(SelectedIndex + (delta < 0.0f ? 1 : -1), 0, count - 1);
            SetSelectedIndex(next);
            return FReply::Handled();
        }

        (void)absolutePos;
        const int32_t count   = static_cast<int32_t>(Options.size());
        const int32_t visible = GetVisibleItemCount();
        const int32_t maxTop  = std::max(0, count - visible);
        m_DropTopIndex = std::clamp(m_DropTopIndex + (delta < 0.0f ? 1 : -1), 0, maxTop);
        return FReply::Handled();
    }

    FReply OnKeyDown(uint32_t keyCode, bool /*repeat*/) override
    {
        // Virtual-key codes, matching what the Win32 host forwards. Named with a
        // k prefix because winuser.h #defines VK_RETURN / VK_ESCAPE / ...
        constexpr uint32_t kReturn = 0x0D;
        constexpr uint32_t kEscape = 0x1B;
        constexpr uint32_t kUp     = 0x26;
        constexpr uint32_t kDown   = 0x28;

        if (!m_Open)
        {
            if (keyCode == kReturn || keyCode == kDown) { SetOpen(true); return FReply::Handled(); }
            return FReply::Unhandled();
        }

        switch (keyCode)
        {
        case kEscape: SetOpen(false); return FReply::Handled();
        case kReturn:
            if (m_HighlightIndex >= 0)
            {
                SetOpen(false);
                SetSelectedIndex(m_HighlightIndex);
            }
            return FReply::Handled();
        case kUp:   MoveHighlight(-1); return FReply::Handled();
        case kDown: MoveHighlight(+1); return FReply::Handled();
        default: break;
        }
        return FReply::Unhandled();
    }

private:
    // ---- geometry helpers ---------------------------------------------------

    UIRect GetBoxRect() const { return GetCachedGeometry().ToRect(); }

    float GetDropDownHeight() const
    {
        const float want = ItemHeight * static_cast<float>(Options.size()) + 4.0f;
        return std::min(MaxDropDownHeight, want);
    }

    UIRect GetDropDownRect() const
    {
        const UIRect box = GetBoxRect();
        return UIRect(box.x, box.y + box.h, box.w, GetDropDownHeight());
    }

    UIRect GetItemRect(int32_t index) const
    {
        const UIRect drop = GetDropDownRect();
        const float  y    = drop.y + 2.0f
                          + static_cast<float>(index - m_DropTopIndex) * ItemHeight;
        return UIRect(drop.x + 1.0f, y, std::max(0.0f, drop.w - 2.0f), ItemHeight);
    }

    int32_t HitTestDropDownItem(const Vector2& absolutePos) const
    {
        const UIRect drop = GetDropDownRect();
        if (!drop.Contains(absolutePos)) return -1;

        const int32_t count = static_cast<int32_t>(Options.size());
        const int32_t rel   = static_cast<int32_t>(std::floor((absolutePos.y - drop.y - 2.0f) / ItemHeight));
        if (rel < 0) return -1;

        const int32_t index = m_DropTopIndex + rel;
        return (index >= 0 && index < count) ? index : -1;
    }

    int32_t GetVisibleItemCount() const
    {
        const int32_t count   = static_cast<int32_t>(Options.size());
        const int32_t visible = std::max(1, static_cast<int32_t>(std::floor(GetDropDownHeight() / ItemHeight)));
        return std::min(count, visible);
    }

    void ClampDropDownScroll()
    {
        const int32_t maxTop = std::max(0, static_cast<int32_t>(Options.size()) - GetVisibleItemCount());
        m_DropTopIndex = std::clamp(m_DropTopIndex, 0, maxTop);
    }

    void MoveHighlight(int32_t delta)
    {
        const int32_t count = static_cast<int32_t>(Options.size());
        if (count == 0) return;
        m_HighlightIndex = std::clamp(m_HighlightIndex + delta, 0, count - 1);
        ScrollHighlightIntoView();
    }

    void ScrollHighlightIntoView()
    {
        const int32_t visible = GetVisibleItemCount();
        if (visible <= 0) return;

        if (m_HighlightIndex < m_DropTopIndex)
            m_DropTopIndex = m_HighlightIndex;
        else if (m_HighlightIndex >= m_DropTopIndex + visible)
            m_DropTopIndex = m_HighlightIndex - visible + 1;

        ClampDropDownScroll();
    }

    bool    m_Open           = false;
    int32_t m_HighlightIndex = -1;
    int32_t m_DropTopIndex   = 0;
};

} // namespace ZSlate
