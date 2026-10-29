#pragma once

#include "ZSlate/Widgets/SCompoundWidget.h"
#include "ZSlate/Widgets/Text/STextBlock.h"

#include <functional>
#include <string>

namespace ZSlate
{

// =============================================================================
// SSearchBox — search input widget (UE SSearchBox analogue)
// =============================================================================
//
// A single-line text box with a magnifying glass icon and optional
// clear button. Calls OnTextChanged on every keystroke and OnTextCommitted
// on Enter.
//
class SSearchBox : public SCompoundWidget
{
public:
    std::string Text;
    std::string HintText {"Search..."};

    float FontSize {14.0f};

    // Callbacks
    std::function<void(const std::string&)> OnTextChanged;
    std::function<void(const std::string&)> OnTextCommitted;

    Vector2 ComputeDesiredSize() const override { return Vector2(200.0f, 28.0f); }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (!ctx.Renderer) return;
        const UIRect r = geom.ToRect();

        // Background (lighter when focused)
        ctx.Renderer->DrawQuad(r, m_Focused
            ? UIColor(0.20f, 0.22f, 0.28f, 1.0f)
            : UIColor(0.14f, 0.14f, 0.18f, 1.0f));
        ctx.Renderer->DrawRect(r, m_Focused
            ? UIColor(0.45f, 0.55f, 0.80f, 1.0f)
            : UIColor(0.30f, 0.32f, 0.38f, 1.0f), 1.0f);

        // Magnifying glass icon
        float iconW = 24.0f;
        ctx.Renderer->DrawTextLabel(
            UIRect(r.x + 4.0f, r.y, iconW, r.h),
            "\xF0\x9F\x94\x8D", FontSize - 2.0f,  // 🔍
            UIColor(0.6f, 0.6f, 0.6f, 1.0f),
            TextAnchor::MiddleCenter, TextWrapMode::NoWrap);

        // Text or hint
        float textX = r.x + iconW + 4.0f;
        float textW = r.w - iconW - 12.0f;

        if (!Text.empty())
        {
            // Cursor blink
            if (m_Focused && (static_cast<int>(m_CursorBlink) % 60 < 30))
            {
                // Simple caret: underscore at end of text
                std::string display = Text + "|";
                ctx.Renderer->DrawTextLabel(
                    UIRect(textX, r.y, textW, r.h),
                    display, FontSize,
                    UIColor(0.90f, 0.90f, 0.95f, 1.0f),
                    TextAnchor::MiddleLeft, TextWrapMode::NoWrap);
            }
            else
            {
                ctx.Renderer->DrawTextLabel(
                    UIRect(textX, r.y, textW, r.h),
                    Text, FontSize,
                    UIColor(0.90f, 0.90f, 0.95f, 1.0f),
                    TextAnchor::MiddleLeft, TextWrapMode::NoWrap);
            }
        }
        else if (!HintText.empty())
        {
            ctx.Renderer->DrawTextLabel(
                UIRect(textX, r.y, textW, r.h),
                HintText, FontSize,
                UIColor(0.45f, 0.45f, 0.50f, 1.0f),
                TextAnchor::MiddleLeft, TextWrapMode::NoWrap);
        }

        // Clear button (×)
        if (!Text.empty())
        {
            float clearX = r.Right() - 22.0f;
            ctx.Renderer->DrawTextLabel(
                UIRect(clearX, r.y, 18.0f, r.h),
                "\xC3\x97", FontSize + 2.0f,  // ×
                UIColor(0.6f, 0.6f, 0.6f, 1.0f),
                TextAnchor::MiddleCenter, TextWrapMode::NoWrap);
        }
    }

    // ---- Input ---------------------------------------------------------------

    bool SupportsKeyboardFocus() const override { return true; }

    void OnFocusReceived() override { m_Focused = true; m_CursorBlink = 0; }
    void OnFocusLost() override { m_Focused = false; }

    FReply OnMouseButtonDown(const Vector2& pos, int btn) override
    {
        if (btn != 0) return FReply::Unhandled();
        const UIRect r = GetCachedGeometry().ToRect();

        // Clear button hit?
        if (!Text.empty() && pos.x > r.Right() - 22.0f)
        {
            Text.clear();
            if (OnTextChanged) OnTextChanged(Text);
            return FReply::Handled();
        }

        return FReply::Handled();  // focus
    }

    void OnKeyChar(unsigned int codepoint) override
    {
        if (!m_Focused) return;

        if (codepoint == '\b' || codepoint == 127)  // Backspace
        {
            if (!Text.empty())
            {
                // Remove last UTF-8 char
                while (!Text.empty() && (Text.back() & 0xC0) == 0x80)
                    Text.pop_back();
                if (!Text.empty())
                    Text.pop_back();
                if (OnTextChanged) OnTextChanged(Text);
            }
        }
        else if (codepoint == '\r' || codepoint == '\n')  // Enter
        {
            if (OnTextCommitted) OnTextCommitted(Text);
        }
        else if (codepoint >= 32)  // Printable
        {
            // Encode codepoint as UTF-8
            if (codepoint < 0x80)
                Text += static_cast<char>(codepoint);
            else if (codepoint < 0x800)
            {
                Text += static_cast<char>(0xC0 | (codepoint >> 6));
                Text += static_cast<char>(0x80 | (codepoint & 0x3F));
            }
            else if (codepoint < 0x10000)
            {
                Text += static_cast<char>(0xE0 | (codepoint >> 12));
                Text += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                Text += static_cast<char>(0x80 | (codepoint & 0x3F));
            }
            else
            {
                Text += static_cast<char>(0xF0 | (codepoint >> 18));
                Text += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
                Text += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                Text += static_cast<char>(0x80 | (codepoint & 0x3F));
            }
            if (OnTextChanged) OnTextChanged(Text);
        }
        m_CursorBlink = 0;
    }

    void Tick(const FGeometry&, float deltaSec) override
    {
        m_CursorBlink += deltaSec * 60.0f;
    }

private:
    mutable bool   m_Focused {false};
    mutable float  m_CursorBlink {0.0f};
};

}  // namespace ZSlate
