#include "ZSlate/Widgets/Input/SEditableTextBox.h"

#include "ZSlate/Core/SlatePlatform.h"

#include <algorithm>

namespace ZSlate
{

namespace
{
    constexpr uint32_t KEY_BACK   = 0x08;
    constexpr uint32_t KEY_TAB    = 0x09;
    constexpr uint32_t KEY_RETURN = 0x0D;
    constexpr uint32_t KEY_ESCAPE = 0x1B;
    constexpr uint32_t KEY_HOME   = 0x24;
    constexpr uint32_t KEY_END    = 0x23;
    constexpr uint32_t KEY_LEFT   = 0x25;
    constexpr uint32_t KEY_RIGHT  = 0x27;
    constexpr uint32_t KEY_DELETE = 0x2E;

    constexpr float TEXT_PADDING = 6.0f;
}

void SEditableTextBox::SetText(const std::string& text)
{
    Text = text;
    m_CaretPos = -1;
    m_ScrollX  = 0.0f;
}

void SEditableTextBox::ClampCursor()
{
    const int32_t length = static_cast<int32_t>(Utf8::Length(Text));
    if (m_CaretPos < 0 || m_CaretPos > length) m_CaretPos = length;
}

Vector2 SEditableTextBox::ComputeDesiredSize() const
{
    const Vector2 textSize = MeasureText(Text.empty() ? HintText : Text, FontSize);
    return Vector2(std::max(MinWidth, textSize.x + TEXT_PADDING * 2.0f),
                   FontSize * 1.4f + TEXT_PADDING * 2.0f);
}

void SEditableTextBox::OnPaint(const FPaintContext& context, const FGeometry& geometry) const
{
    if (!context.Renderer) return;

    const UIRect r = geometry.ToRect();

    context.Renderer->DrawQuad(r, BackgroundColor);
    context.Renderer->DrawRect(r, HasKeyboardFocus() ? FocusedBorderColor : BorderColor, 1.0f);

    const bool hasText = !Text.empty();

    // Caret position (code-point index) and its pixel offset.
    const int32_t caret  = (m_CaretPos < 0)
        ? static_cast<int32_t>(Utf8::Length(Text)) : m_CaretPos;
    const size_t  caretByte = Utf8::ByteOffsetOf(Text, static_cast<size_t>(caret));
    const float   caretX = MeasureText(Text.substr(0, caretByte), FontSize).x;
    const float   textW  = MeasureText(Text, FontSize).x;

    const UIRect viewRect(r.x + 1.0f, r.y + 1.0f, r.w - 2.0f, r.h - 2.0f);
    const float  viewW   = viewRect.w - TEXT_PADDING * 2.0f;

    // Keep the caret inside the visible window.
    if (caretX - m_ScrollX > viewW)  m_ScrollX = caretX - viewW;
    if (caretX - m_ScrollX < 0.0f)   m_ScrollX = caretX;
    if (m_ScrollX < 0.0f)            m_ScrollX = 0.0f;

    context.Renderer->PushClipRect(viewRect);

    const UIRect textRect(viewRect.x + TEXT_PADDING - m_ScrollX,
                          viewRect.y,
                          std::max(textW, viewW),
                          viewRect.h);

    if (hasText)
    {
        context.Renderer->DrawTextLabel(textRect, Text, FontSize, TextColor,
                                        TextAnchor::MiddleLeft, TextWrapMode::NoWrap, nullptr);
    }
    else if (!HintText.empty())
    {
        context.Renderer->DrawTextLabel(textRect, HintText, FontSize, HintColor,
                                        TextAnchor::MiddleLeft, TextWrapMode::NoWrap, nullptr);
    }

    if (HasKeyboardFocus() && IsCaretVisible())
    {
        const UIRect caretRect(textRect.x + caretX,
                               viewRect.y + 4.0f,
                               1.5f,
                               std::max(0.0f, viewRect.h - 8.0f));
        context.Renderer->DrawQuad(caretRect, TextColor);
    }

    context.Renderer->PopClipRect();
}

FReply SEditableTextBox::OnMouseButtonDown(const Vector2& absolutePos, int button)
{
    (void)absolutePos; (void)button;
    return FReply::Handled();   // the router grants focus on mouse-down
}

void SEditableTextBox::OnFocusLost()
{
    m_ScrollX = 0.0f;
}

FReply SEditableTextBox::OnKeyChar(uint32_t codepoint)
{
    if (codepoint < 0x20 || codepoint == 0x7F) return FReply::Unhandled();

    ClampCursor();
    const size_t byteOffset = Utf8::ByteOffsetOf(Text, static_cast<size_t>(m_CaretPos));

    std::string encoded;
    Utf8::Append(encoded, codepoint);
    Text.insert(byteOffset, encoded);

    m_CaretPos = static_cast<int32_t>(Utf8::Length(Text.substr(0, byteOffset + encoded.size())));

    if (OnTextChanged) OnTextChanged(Text);
    return FReply::Handled();
}

FReply SEditableTextBox::OnKeyDown(uint32_t keyCode, bool repeat)
{
    (void)repeat;
    ClampCursor();

    const int32_t length = static_cast<int32_t>(Utf8::Length(Text));

    switch (keyCode)
    {
    case KEY_LEFT:
        if (m_CaretPos > 0) --m_CaretPos;
        return FReply::Handled();

    case KEY_RIGHT:
        if (m_CaretPos < length) ++m_CaretPos;
        return FReply::Handled();

    case KEY_HOME:
        m_CaretPos = 0;
        return FReply::Handled();

    case KEY_END:
        m_CaretPos = length;
        return FReply::Handled();

    case KEY_BACK:
    {
        if (m_CaretPos > 0 && !Text.empty())
        {
            const size_t start = Utf8::ByteOffsetOf(Text, static_cast<size_t>(m_CaretPos - 1));
            const size_t end   = Utf8::ByteOffsetOf(Text, static_cast<size_t>(m_CaretPos));
            Text.erase(start, end - start);
            m_CaretPos = static_cast<int32_t>(Utf8::Length(Text.substr(0, start)));
            if (OnTextChanged) OnTextChanged(Text);
        }
        return FReply::Handled();
    }

    case KEY_DELETE:
    {
        if (m_CaretPos < length && !Text.empty())
        {
            const size_t start = Utf8::ByteOffsetOf(Text, static_cast<size_t>(m_CaretPos));
            const size_t end   = Utf8::ByteOffsetOf(Text, static_cast<size_t>(m_CaretPos + 1));
            Text.erase(start, end - start);
            if (OnTextChanged) OnTextChanged(Text);
        }
        return FReply::Handled();
    }

    case KEY_RETURN:
        if (OnEnterPressed) OnEnterPressed();
        return FReply::Handled();

    case KEY_ESCAPE:
    case KEY_TAB:
        return FReply::Unhandled();

    default:
        return FReply::Unhandled();
    }
}

} // namespace ZSlate
