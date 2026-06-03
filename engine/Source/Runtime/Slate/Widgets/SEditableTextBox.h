#pragma once

#include "Runtime/Slate/Widgets/SLeafWidget.h"

#include <functional>
#include <string>

namespace ZSlate
{
// A single-line text input (UE Slate SEditableTextBox analogue). Click to focus,
// type to edit (UTF-8 aware), Backspace deletes, Enter commits. Caret is drawn at
// the end of the text while focused. Caret movement / selection are future work.
class SEditableTextBox : public SLeafWidget
{
public:
    std::string Text;
    std::string HintText;
    float FontSize {16.0f};
    float MinWidth {140.0f};
    FMargin Padding {8.0f, 4.0f};

    UIColor BackgroundColor {0.08f, 0.08f, 0.10f, 1.0f};
    UIColor BorderColor {0.35f, 0.36f, 0.40f, 1.0f};
    UIColor FocusBorderColor {0.36f, 0.62f, 0.96f, 1.0f};
    UIColor TextColor {0.92f, 0.93f, 0.96f, 1.0f};
    UIColor HintColor {0.45f, 0.46f, 0.52f, 1.0f};

    std::function<void(const std::string&)> OnTextChanged;
    std::function<void(const std::string&)> OnTextCommitted;

    Vector2 ComputeDesiredSize() const override
    {
        float text_w = 0.0f;
        if (GSlateTextMeasurer && !Text.empty())
            text_w = GSlateTextMeasurer->Measure(Text, FontSize).x;
        const float width = (text_w + Padding.GetTotalHorizontal() + FontSize) ;
        const float w = width > MinWidth ? width : MinWidth;
        const float h = FontSize * 1.3f + Padding.GetTotalVertical();
        return Vector2(w, h);
    }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (ctx.Renderer == nullptr)
            return;
        const UIRect rect = geom.ToRect();
        ctx.Renderer->drawQuad(rect, BackgroundColor);
        ctx.Renderer->drawRect(rect, m_Focused ? FocusBorderColor : BorderColor, 1.0f);

        const UIRect text_rect(rect.x + Padding.Left,
                               rect.y + Padding.Top,
                               rect.width - Padding.GetTotalHorizontal(),
                               rect.height - Padding.GetTotalVertical());

        if (Text.empty() && !m_Focused && !HintText.empty())
        {
            ctx.Renderer->drawText(text_rect, HintText, FontSize, HintColor, TextAnchor::MiddleLeft,
                                   TextWrapMode::NoWrap, nullptr);
        }
        else
        {
            ctx.Renderer->drawText(text_rect, Text, FontSize, TextColor, TextAnchor::MiddleLeft,
                                   TextWrapMode::NoWrap, nullptr);
        }

        if (m_Focused)
        {
            float caret_x = text_rect.x;
            if (!Text.empty())
                caret_x += ctx.Renderer->measureText(Text, FontSize, TextWrapMode::NoWrap, 0.0f, nullptr).x;
            const float caret_h = FontSize;
            const float caret_y = rect.y + (rect.height - caret_h) * 0.5f;
            ctx.Renderer->drawQuad(UIRect(caret_x + 1.0f, caret_y, 1.5f, caret_h), TextColor);
        }
    }

    bool SupportsKeyboardFocus() const override { return true; }
    void OnFocusReceived() override { m_Focused = true; }
    void OnFocusLost() override { m_Focused = false; }

    // Hosts use this to avoid clobbering the buffer while the user is typing.
    bool IsFocused() const { return m_Focused; }

    FReply OnMouseButtonDown(const Vector2& /*pos*/, int button) override
    {
        // Handle so the click is consumed (focus is set by the router separately).
        return (button == 0) ? FReply::Handled() : FReply::Unhandled();
    }

    void OnKeyChar(unsigned int codepoint) override
    {
        if (codepoint < 32 || codepoint == 127)
            return;  // control chars handled via OnKeyDown
        AppendUtf8(Text, codepoint);
        if (OnTextChanged)
            OnTextChanged(Text);
    }

    FReply OnKeyDown(EKey key) override
    {
        switch (key)
        {
            case EKey::Backspace:
                if (!Text.empty())
                {
                    PopUtf8(Text);
                    if (OnTextChanged)
                        OnTextChanged(Text);
                }
                return FReply::Handled();
            case EKey::Enter:
                if (OnTextCommitted)
                    OnTextCommitted(Text);
                return FReply::Handled();
            default:
                return FReply::Unhandled();
        }
    }

private:
    static void AppendUtf8(std::string& s, unsigned int cp)
    {
        if (cp < 0x80)
        {
            s.push_back(static_cast<char>(cp));
        }
        else if (cp < 0x800)
        {
            s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else if (cp < 0x10000)
        {
            s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else
        {
            s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    static void PopUtf8(std::string& s)
    {
        if (s.empty())
            return;
        size_t i = s.size();
        do
        {
            --i;
        } while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80);
        s.erase(i);
    }

    bool m_Focused {false};
};
}  // namespace ZSlate
