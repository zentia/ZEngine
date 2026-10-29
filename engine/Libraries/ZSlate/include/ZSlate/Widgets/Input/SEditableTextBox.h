#pragma once

#define ZSLATE_SEDITABLETEXTBOX_DEFINED

#include "ZSlate/Widgets/SWidget.h"
#include "ZSlate/Core/SlateClipboard.h"
#include "ZSlate/Core/SlatePaint.h"      // GSlateTextMeasurer (char-offset hit testing)
#include "ZSlate/Application/SlateInput.h"  // SlateInputRouter::IsCtrlDown / IsShiftDown

#include <functional>
#include <string>
#include <algorithm>

namespace ZSlate
{

// =============================================================================
// SEditableTextBox — single-line text input (UE SEditableTextBox analogue)
// =============================================================================

class SEditableTextBox : public SWidget
{
public:
    std::string Text;
    std::string HintText;
    float FontSize {16.0f};
    float MinWidth {140.0f};
    FMargin Padding;

    std::function<void(const std::string&)> OnTextChanged;
    std::function<void(const std::string&)> OnTextCommitted;

    void SetText(const std::string& t)        { Text = t; m_CursorPos = t.size(); }
    const std::string& GetText() const        { return Text; }
    void SetHintText(const std::string& h)     { HintText = h; }
    bool IsFocused() const                     { return m_Focused; }

    Vector2 ComputeDesiredSize() const override
    {
        return Vector2(std::max(MinWidth,
            static_cast<float>(Text.length()) * FontSize * 0.6f + 12.0f),
            FontSize + 10.0f);
    }

    void ArrangeChildren(const FGeometry& geom,
                         std::vector<FArrangedWidget>& out) const override
    {
        (void)geom; (void)out;
    }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (!ctx.Renderer) return;
        // Cache geometry so mouse hit-testing (screen -> widget-local) works.
        m_CachedGeometry = geom;
        const UIRect rect = geom.ToRect();

        // Background
        ctx.Renderer->DrawQuad(rect, m_Focused
            ? UIColor(0.18f, 0.19f, 0.24f, 1.0f)
            : UIColor(0.12f, 0.12f, 0.15f, 1.0f));

        // Border
        ctx.Renderer->DrawRect(rect, m_Focused
            ? UIColor(0.45f, 0.55f, 0.80f, 1.0f)
            : UIColor(0.30f, 0.32f, 0.38f, 1.0f), 1.0f);

        // Text or hint
        UIRect textRect(rect.x + Padding.Left + 4.0f, rect.y + Padding.Top,
                        rect.w - Padding.GetTotalHorizontal() - 8.0f,
                        rect.h - Padding.GetTotalVertical());

        if (!Text.empty())
        {
            std::string display = Text;
            const size_t c0 = std::min(m_SelStart, m_SelEnd);
            const size_t c1 = std::max(m_SelStart, m_SelEnd);
            const bool has_sel = m_Focused && (c0 != c1);

            // Selection highlight (drawn below the text).
            if (has_sel && GSlateTextMeasurer != nullptr)
            {
                const float x0 = textRect.x + MeasurePrefixWidth(Text.substr(0, c0), FontSize);
                const float x1 = textRect.x + MeasurePrefixWidth(Text.substr(0, c1), FontSize);
                UIRect selRect(std::min(x0, x1), textRect.y, std::abs(x1 - x0), textRect.h);
                ctx.Renderer->DrawQuad(selRect, UIColor(0.20f, 0.45f, 0.85f, 0.85f));
            }

            // Insert cursor if focused (blink) and not replaced by a selection block.
            if (m_Focused && (m_CursorBlinkPhase < 0.5f))
            {
                if (has_sel)
                {
                    // Overwrite the selected region with the blinking caret.
                    const float cx = textRect.x + MeasurePrefixWidth(Text.substr(0, c0), FontSize);
                    ctx.Renderer->DrawRect(UIRect(cx, textRect.y, 1.5f, textRect.h),
                                           UIColor(0.90f, 0.90f, 0.95f, 1.0f), 1.0f);
                }
                else
                {
                    size_t pos = std::min(m_CursorPos, display.size());
                    display.insert(pos, "|");
                }
            }

            ctx.Renderer->DrawTextLabel(
                textRect, display, FontSize,
                UIColor(0.90f, 0.90f, 0.95f, 1.0f),
                TextAnchor::MiddleLeft, TextWrapMode::NoWrap);
        }
        else if (!HintText.empty())
        {
            ctx.Renderer->DrawTextLabel(
                textRect, HintText, FontSize,
                UIColor(0.45f, 0.45f, 0.50f, 1.0f),
                TextAnchor::MiddleLeft, TextWrapMode::NoWrap);
        }
    }

    // ---- Input ---------------------------------------------------------------

    bool SupportsKeyboardFocus() const override { return true; }
    void OnFocusReceived() override { m_Focused = true; m_CursorBlinkPhase = 0; }
    void OnFocusLost() override { m_Focused = false; ClearSelection(); }

    FReply OnMouseButtonDown(const Vector2& screen_pos, int btn) override
    {
        if (btn != 0) return FReply::Unhandled();
        // Focus + begin a drag-selection.
        const size_t hit = HitTest(screen_pos);
        if (SlateInputRouter::IsCtrlDown())
        {
            // Ctrl+click: extend selection to the clicked position.
            m_SelAnchor = m_CursorPos;
            m_SelStart = std::min(m_SelAnchor, hit);
            m_SelEnd = std::max(m_SelAnchor, hit);
        }
        else
        {
            // Plain click: place caret, clear selection.
            m_CursorPos = hit;
            ClearSelection();
            m_SelAnchor = hit;
        }
        m_Dragging = true;
        return FReply::Handled();  // gain focus (router sets focus from path)
    }

    FReply OnMouseButtonUp(const Vector2& /*screen_pos*/, int /*button*/) override
    {
        if (m_Dragging)
        {
            m_Dragging = false;
            return FReply::Handled();
        }
        return FReply::Unhandled();
    }

    void OnMouseMove(const Vector2& screen_pos) override
    {
        // Only fires for the captured widget during a drag (the router delivers
        // moves to the captor; hover-path widgets skip the captured one).
        if (!m_Dragging) return;
        const size_t hit = HitTest(screen_pos);
        m_SelStart = std::min(m_SelAnchor, hit);
        m_SelEnd = std::max(m_SelAnchor, hit);
    }

    void OnMouseCaptureLost() override { m_Dragging = false; }

    // Map a screen-x to a UTF-8 char offset using the global text measurer.
    size_t HitTest(const Vector2& screen_pos) const
    {
        if (Text.empty() || GSlateTextMeasurer == nullptr)
            return m_CursorPos;

        // Convert screen -> widget-local x (mirrors SMultiLineEditableText).
        const float local_x = screen_pos.x - m_CachedGeometry.AbsolutePosition.x
                              - Padding.Left - 4.0f;
        if (local_x <= 0.0f) return 0;

        // Binary search over char boundaries.
        size_t lo = 0, hi = Text.size();
        while (lo < hi)
        {
            const size_t mid = (lo + hi) / 2;
            const float w = GSlateTextMeasurer->Measure(Text.substr(0, mid), FontSize).x;
            if (w < local_x) lo = mid + 1; else hi = mid;
        }
        // Snap to a whole UTF-8 code point so we never split a multibyte char.
        while (lo > 0 && (static_cast<unsigned char>(Text[lo]) & 0xC0) == 0x80) --lo;
        return lo;
    }

    void OnKeyChar(unsigned int codepoint) override
    {
        if (!m_Focused) return;

        if (codepoint == '\b' || codepoint == 127)  // Backspace
        {
            if (HasSelection())
            {
                DeleteSelection();
            }
            else if (m_CursorPos > 0)
            {
                // Remove one UTF-8 char before cursor
                size_t del = m_CursorPos - 1;
                while (del > 0 && (Text[del] & 0xC0) == 0x80) --del;
                size_t len = m_CursorPos - del;
                Text.erase(del, len);
                m_CursorPos = del;
                if (OnTextChanged) OnTextChanged(Text);
            }
        }
        else if (codepoint == 127 || codepoint == 0x7F)  // Delete
        {
            if (HasSelection())
            {
                DeleteSelection();
            }
            else if (m_CursorPos < Text.size())
            {
                size_t end = m_CursorPos + 1;
                while (end < Text.size() && (Text[end] & 0xC0) == 0x80) ++end;
                Text.erase(m_CursorPos, end - m_CursorPos);
                if (OnTextChanged) OnTextChanged(Text);
            }
        }
        else if (codepoint == '\r' || codepoint == '\n')  // Enter
        {
            if (OnTextCommitted) OnTextCommitted(Text);
        }
        else if (codepoint >= 32)  // Printable
        {
            if (HasSelection()) DeleteSelection();

            // Encode as UTF-8
            std::string utf8;
            if (codepoint < 0x80)
                utf8 += static_cast<char>(codepoint);
            else if (codepoint < 0x800)
            {
                utf8 += static_cast<char>(0xC0 | (codepoint >> 6));
                utf8 += static_cast<char>(0x80 | (codepoint & 0x3F));
            }
            else if (codepoint < 0x10000)
            {
                utf8 += static_cast<char>(0xE0 | (codepoint >> 12));
                utf8 += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                utf8 += static_cast<char>(0x80 | (codepoint & 0x3F));
            }
            else
            {
                utf8 += static_cast<char>(0xF0 | (codepoint >> 18));
                utf8 += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
                utf8 += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                utf8 += static_cast<char>(0x80 | (codepoint & 0x3F));
            }
            Text.insert(m_CursorPos, utf8);
            m_CursorPos += utf8.size();
            ClearSelection();
            if (OnTextChanged) OnTextChanged(Text);
        }
        m_CursorBlinkPhase = 0.0f;
    }

    // Ctrl+C/V/X/A keyboard shortcuts. Routed here (not OnKeyChar) because
    // combination keys never produce a character codepoint. Mirrors the
    // SMultiLineEditableText bindings so behaviour is consistent across widgets.
    FReply OnKeyDown(EKey key) override
    {
        if (!m_Focused) return FReply::Unhandled();
        if (!SlateInputRouter::IsCtrlDown()) return FReply::Unhandled();

        switch (key)
        {
            case EKey::A:
                SelectAll();
                return FReply::Handled();
            case EKey::C:
                CopySelection();
                return FReply::Handled();
            case EKey::V:
                PasteText();
                return FReply::Handled();
            case EKey::X:
                CutSelection();
                return FReply::Handled();
            default:
                return FReply::Unhandled();
        }
    }

    void Tick(const FGeometry&, float deltaSec) override
    {
        if (m_Focused)
            m_CursorBlinkPhase = std::fmod(m_CursorBlinkPhase + deltaSec * 2.0f, 1.0f);
    }

    // ---- Selection / clipboard helpers ---------------------------------------

    bool HasSelection() const { return m_SelStart != m_SelEnd; }

    void ClearSelection() { m_SelStart = m_SelEnd = m_CursorPos; }

    void SelectAll()
    {
        m_SelStart = 0;
        m_SelEnd = Text.size();
    }

    std::string GetSelectedText() const
    {
        if (!HasSelection()) return "";
        const size_t a = std::min(m_SelStart, m_SelEnd);
        const size_t b = std::max(m_SelStart, m_SelEnd);
        return Text.substr(a, b - a);
    }

    void CopySelection()
    {
        const std::string sel = GetSelectedText();
        if (sel.empty()) return;
        if (GSlateSetClipboard) GSlateSetClipboard(sel.c_str(), GSlateClipboardUserData);
    }

    void CutSelection()
    {
        if (!HasSelection()) return;
        CopySelection();
        DeleteSelection();
        if (OnTextChanged) OnTextChanged(Text);
    }

    void PasteText()
    {
        if (!GSlateGetClipboard) return;
        const char* clip = GSlateGetClipboard(GSlateClipboardUserData);
        if (clip == nullptr || *clip == '\0') return;
        if (HasSelection()) DeleteSelection();
        Text.insert(m_CursorPos, clip);
        m_CursorPos += std::string(clip).size();
        ClearSelection();
        if (OnTextChanged) OnTextChanged(Text);
    }

    void DeleteSelection()
    {
        if (!HasSelection()) return;
        const size_t a = std::min(m_SelStart, m_SelEnd);
        const size_t b = std::max(m_SelStart, m_SelEnd);
        Text.erase(a, b - a);
        m_CursorPos = a;
        ClearSelection();
    }

    // Width of a text prefix in widget-local pixels (0 if no measurer).
    float MeasurePrefixWidth(const std::string& prefix, float font_size) const
    {
        if (prefix.empty() || GSlateTextMeasurer == nullptr) return 0.0f;
        return GSlateTextMeasurer->Measure(prefix, font_size).x;
    }

private:
    mutable bool   m_Focused {false};
    mutable size_t m_CursorPos {0};
    mutable float  m_CursorBlinkPhase {0.0f};
    // Cached painted geometry for screen->local hit testing (see HitTest).
    mutable FGeometry m_CachedGeometry;
    // Selection model (char offsets; m_SelAnchor is the fixed drag start).
    mutable size_t m_SelStart {0};
    mutable size_t m_SelEnd {0};
    mutable size_t m_SelAnchor {0};
    mutable bool   m_Dragging {false};
};

}  // namespace ZSlate
