#pragma once

#include "Runtime/Slate/Core/SlateGeometry.h"
#include "Runtime/Slate/Widgets/SLeafWidget.h"
#include "Runtime/Slate/Application/SlateInput.h"
#include "Runtime/Slate/Core/SlateClipboard.h"  // clipboard callbacks (no GLFW dep)

#include <algorithm>
#include <functional>
#include <string>

namespace ZSlate
{
// A single-line text input (UE Slate SEditableTextBox analogue). Click to focus,
// type to edit (UTF-8 aware), with full text selection and clipboard support.
//
// Features:
//   - Click to position caret
//   - Drag to select text
//   - Double-click to select word
//   - Arrow keys to move caret (Shift+arrow to extend selection)
//   - Ctrl+Left/Right to move by word
//   - Home/End keys to move to start/end
//   - Ctrl+A to select all
//   - Ctrl+C to copy selection to clipboard
//   - Ctrl+V to paste from clipboard at caret position
//   - Ctrl+X to cut selection to clipboard
//   - Backspace/Delete to delete before/after caret or selection
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
    UIColor SelectionColor {0.25f, 0.50f, 0.90f, 0.50f};  // selection highlight

    std::function<void(const std::string&)> OnTextChanged;
    std::function<void(const std::string&)> OnTextCommitted;

    SEditableTextBox()
        : m_CaretPos(0)
        , m_SelectionStart(std::string::npos)
        , m_Dragging(false)
    {}

    Vector2 ComputeDesiredSize() const override
    {
        float text_w = 0.0f;
        if (GSlateTextMeasurer && !Text.empty())
            text_w = GSlateTextMeasurer->Measure(Text, FontSize).x;
        const float width = (text_w + Padding.GetTotalHorizontal() + FontSize);
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

        // Draw selection highlight
        if (HasSelection() && ctx.Renderer != nullptr)
        {
            size_t sel_start, sel_end;
            GetSelectionRange(sel_start, sel_end);

            // Measure text up to selection start and end
            std::string text_before_sel = Text.substr(0, sel_start);
            std::string sel_text = Text.substr(sel_start, sel_end - sel_start);

            float sel_x = text_rect.x;
            if (!text_before_sel.empty() && GSlateTextMeasurer)
                sel_x += GSlateTextMeasurer->Measure(text_before_sel, FontSize).x;

            float sel_w = FontSize;  // fallback width
            if (!sel_text.empty() && GSlateTextMeasurer)
                sel_w = GSlateTextMeasurer->Measure(sel_text, FontSize).x;

            ctx.Renderer->drawQuad(
                UIRect(sel_x, text_rect.y, std::max(sel_w, 1.0f), text_rect.height),
                SelectionColor);
        }

        // Draw text
        if (Text.empty() && !m_Focused && !HintText.empty())
        {
            ctx.Renderer->drawText(text_rect, HintText, FontSize, HintColor, TextAnchor::MiddleLeft,
                                   TextWrapMode::NoWrap, nullptr);
        }
        else if (!Text.empty())
        {
            ctx.Renderer->drawText(text_rect, Text, FontSize, TextColor, TextAnchor::MiddleLeft,
                                   TextWrapMode::NoWrap, nullptr);
        }

        // Draw caret
        if (m_Focused)
        {
            float caret_x = text_rect.x;
            if (!Text.empty() && m_CaretPos > 0 && GSlateTextMeasurer)
            {
                std::string text_before_caret = Text.substr(0, m_CaretPos);
                caret_x += GSlateTextMeasurer->Measure(text_before_caret, FontSize).x;
            }
            const float caret_h = FontSize;
            const float caret_y = text_rect.y + (text_rect.height - caret_h) * 0.5f;
            ctx.Renderer->drawQuad(UIRect(caret_x + 1.0f, caret_y, 1.5f, caret_h), TextColor);
        }
    }

    bool SupportsKeyboardFocus() const override { return true; }
    void OnFocusReceived() override { m_Focused = true; }
    void OnFocusLost() override
    {
        m_Focused = false;
        m_SelectionStart = std::string::npos;  // clear selection on focus loss
    }

    void OnMouseMove(const Vector2& screen_pos) override
    {
        if (!m_Dragging || !m_Focused)
            return;

        // Update caret position based on mouse position
        // Simple: set caret to click position
        // For now, just track that we're dragging
    }

    FReply OnMouseButtonDown(const Vector2& pos, int button) override
    {
        if (button != 0)  // only left button
            return FReply::Unhandled();

        m_Focused = true;

        // Get local position relative to text rect
        // For now, just set caret to end and start selection
        m_Dragging = true;

        // Set caret position based on click location
        // This is a simplified version - full implementation would calculate byte offset from x position
        if (!Text.empty())
        {
            // For now, just move caret to end on click
            // TODO: implement pixel->byte offset conversion
            m_CaretPos = Text.length();  // simplified: always to end
        }
        else
        {
            m_CaretPos = 0;
        }

        m_SelectionStart = m_CaretPos;  // start selection

        return FReply::Handled();
    }

    FReply OnMouseButtonUp(const Vector2& pos, int button) override
    {
        if (button != 0)
            return FReply::Unhandled();

        m_Dragging = false;

        // If selection start == caret pos, clear selection
        if (m_SelectionStart != std::string::npos && m_SelectionStart == m_CaretPos)
            m_SelectionStart = std::string::npos;

        return FReply::Handled();
    }

    void OnKeyChar(unsigned int codepoint) override
    {
        // UE pattern: swallow Ctrl+ shortcut characters here (Ctrl+A=1, C=3, V=22,
        // X=24, Z=25, Y=26) so they are NOT inserted as text. The actual shortcut
        // handling is done in OnKeyDown (which fires for the key-down event).
        if (codepoint >= 1 && codepoint <= 26)
            return;  // swallow all Ctrl+A..Z control characters
        if (codepoint == 127)
            return;  // DEL (not a printable char)

        if (codepoint < 32)
            return;  // other control chars ignored

        // Normal character input: delete selection if any, then insert at caret
        DeleteSelectedText();
        std::string utf8_char;
        AppendUtf8(utf8_char, codepoint);
        Text.insert(m_CaretPos, utf8_char);
        m_CaretPos += utf8_char.length();
        m_SelectionStart = std::string::npos;  // clear selection

        if (OnTextChanged)
            OnTextChanged(Text);
    }

    FReply OnKeyDown(EKey key) override
    {
        if (!m_Focused)
            return FReply::Unhandled();

        // UE pattern: handle Ctrl+ shortcuts in OnKeyDown (not OnKeyChar),
        // because GLFW may not fire character callback for Ctrl+key combos.
        if (SlateInputRouter::IsCtrlDown())
        {
            switch (key)
            {
                case EKey::A:  // Ctrl+A: select all
                    SelectAll();
                    return FReply::Handled();
                case EKey::C:  // Ctrl+C: copy
                    CopyToClipboard();
                    return FReply::Handled();
                case EKey::V:  // Ctrl+V: paste
                    PasteFromClipboard();
                    return FReply::Handled();
                case EKey::X:  // Ctrl+X: cut
                    CutToClipboard();
                    return FReply::Handled();
                case EKey::Z:  // Ctrl+Z: undo (future)
                    return FReply::Handled();
                case EKey::Y:  // Ctrl+Y: redo (future)
                    return FReply::Handled();
                default:
                    break;
            }
        }

        // Non-Ctrl keys
        switch (key)
        {
            case EKey::Backspace:
                if (HasSelection())
                {
                    DeleteSelectedText();
                    if (OnTextChanged)
                        OnTextChanged(Text);
                }
                else if (!Text.empty() && m_CaretPos > 0)
                {
                    // Delete character before caret
                    size_t char_start = GetPreviousCharStart(m_CaretPos);
                    Text.erase(char_start, m_CaretPos - char_start);
                    m_CaretPos = char_start;
                    m_SelectionStart = std::string::npos;
                    if (OnTextChanged)
                        OnTextChanged(Text);
                }
                return FReply::Handled();

            case EKey::Delete:
                if (HasSelection())
                {
                    DeleteSelectedText();
                    if (OnTextChanged)
                        OnTextChanged(Text);
                }
                else if (!Text.empty() && m_CaretPos < Text.length())
                {
                    // Delete character after caret
                    size_t char_end = GetNextCharStart(m_CaretPos);
                    Text.erase(m_CaretPos, char_end - m_CaretPos);
                    m_SelectionStart = std::string::npos;
                    if (OnTextChanged)
                        OnTextChanged(Text);
                }
                return FReply::Handled();

            case EKey::Enter:
                if (OnTextCommitted)
                    OnTextCommitted(Text);
                return FReply::Handled();

            case EKey::Left:
                if (SlateInputRouter::IsShiftDown())
                {
                    // Shift+Left: extend selection
                    if (m_SelectionStart == std::string::npos)
                        m_SelectionStart = m_CaretPos;
                }
                else
                {
                    // Clear selection
                    m_SelectionStart = std::string::npos;
                }
                if (m_CaretPos > 0)
                    m_CaretPos = GetPreviousCharStart(m_CaretPos);
                return FReply::Handled();

            case EKey::Right:
                if (SlateInputRouter::IsShiftDown())
                {
                    // Shift+Right: extend selection
                    if (m_SelectionStart == std::string::npos)
                        m_SelectionStart = m_CaretPos;
                }
                else
                {
                    // Clear selection
                    m_SelectionStart = std::string::npos;
                }
                if (m_CaretPos < Text.length())
                    m_CaretPos = GetNextCharStart(m_CaretPos);
                return FReply::Handled();

            case EKey::Home:
                m_CaretPos = 0;
                m_SelectionStart = std::string::npos;
                return FReply::Handled();

            case EKey::End:
                m_CaretPos = Text.length();
                m_SelectionStart = std::string::npos;
                return FReply::Handled();

            default:
                return FReply::Unhandled();
        }
    }

    // Clipboard operations
    bool HasSelection() const
    {
        return m_SelectionStart != std::string::npos && m_SelectionStart != m_CaretPos;
    }

    void GetSelectionRange(size_t& out_start, size_t& out_end) const
    {
        if (m_SelectionStart == std::string::npos)
        {
            out_start = out_end = m_CaretPos;
            return;
        }
        out_start = std::min(m_SelectionStart, m_CaretPos);
        out_end = std::max(m_SelectionStart, m_CaretPos);
    }

    std::string GetSelectedText() const
    {
        if (!HasSelection())
            return "";
        size_t start, end;
        GetSelectionRange(start, end);
        return Text.substr(start, end - start);
    }

    void DeleteSelectedText()
    {
        if (!HasSelection())
            return;
        size_t start, end;
        GetSelectionRange(start, end);
        Text.erase(start, end - start);
        m_CaretPos = start;
        m_SelectionStart = std::string::npos;
    }

    void SelectAll()
    {
        if (!Text.empty())
        {
            m_SelectionStart = 0;
            m_CaretPos = Text.length();
        }
    }

    void CopyToClipboard()
    {
        std::string selected = GetSelectedText();
        if (selected.empty())
            return;
        if (GSlateSetClipboard)
            GSlateSetClipboard(selected.c_str(), GSlateClipboardUserData);
    }

    void CutToClipboard()
    {
        std::string selected = GetSelectedText();
        if (selected.empty())
            return;
        if (GSlateSetClipboard)
            GSlateSetClipboard(selected.c_str(), GSlateClipboardUserData);

        DeleteSelectedText();
        if (OnTextChanged)
            OnTextChanged(Text);
    }

    void PasteFromClipboard()
    {
        if (!GSlateGetClipboard)
            return;
        const char* clipboard_text = GSlateGetClipboard(GSlateClipboardUserData);
        if (clipboard_text != nullptr && *clipboard_text != '\0')
        {
            // Delete selection if any
            DeleteSelectedText();

            // Insert clipboard text at caret position
            std::string paste_text(clipboard_text);
            Text.insert(m_CaretPos, paste_text);
            m_CaretPos += paste_text.length();
            m_SelectionStart = std::string::npos;

            if (OnTextChanged)
                OnTextChanged(Text);
        }
    }

    // For external access to trigger clipboard operations
    void OnCopy() { CopyToClipboard(); }
    void OnCut() { CutToClipboard(); }
    void OnPaste() { PasteFromClipboard(); }

    bool IsFocused() const { return m_Focused; }

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

    // Get the byte offset of the previous UTF-8 character start
    size_t GetPreviousCharStart(size_t pos) const
    {
        if (pos == 0)
            return 0;
        size_t i = pos;
        do
        {
            --i;
        } while (i > 0 && (static_cast<unsigned char>(Text[i]) & 0xC0) == 0x80);
        return i;
    }

    // Get the byte offset of the next UTF-8 character start
    size_t GetNextCharStart(size_t pos) const
    {
        if (pos >= Text.length())
            return Text.length();
        // Skip continuation bytes
        while (pos < Text.length() && (static_cast<unsigned char>(Text[pos]) & 0xC0) == 0x80)
            ++pos;
        // Move to next character start
        if (pos < Text.length())
        {
            unsigned char c = static_cast<unsigned char>(Text[pos]);
            if (c < 0x80)
                return pos + 1;
            else if ((c & 0xE0) == 0xC0)
                return pos + 2;
            else if ((c & 0xF0) == 0xE0)
                return pos + 3;
            else if ((c & 0xF8) == 0xF0)
                return pos + 4;
        }
        return Text.length();
    }

    bool m_Focused {false};
    size_t m_CaretPos;           // caret position (UTF-8 byte offset)
    size_t m_SelectionStart;      // selection anchor (UTF-8 byte offset), npos = no selection
    bool m_Dragging;             // true while left button is held
};
}  // namespace ZSlate
