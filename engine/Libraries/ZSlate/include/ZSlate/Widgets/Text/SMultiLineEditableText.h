#pragma once

#include "ZSlate/Widgets/SWidget.h"
#include "ZSlate/Widgets/SLeafWidget.h"
#include "ZSlate/Application/SlateInput.h"
#include "ZSlate/Core/SlateClipboard.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ZSlate
{
// ============================================================================
// Text Types
// ============================================================================

// Text location (line + column offset)
struct FTextLocation
{
    int32_t LineIndex {0};     // 0-based line index
    int32_t CharIndex {0};     // 0-based character index within line
    
    FTextLocation() = default;
    FTextLocation(int32_t Line, int32_t Char) : LineIndex(Line), CharIndex(Char) {}
    
    bool operator==(const FTextLocation& Other) const
    {
        return LineIndex == Other.LineIndex && CharIndex == Other.CharIndex;
    }
    bool operator!=(const FTextLocation& Other) const { return !(*this == Other); }
    
    FTextLocation operator+(const FTextLocation& Other) const
    {
        return FTextLocation(LineIndex + Other.LineIndex, CharIndex + Other.CharIndex);
    }
    FTextLocation operator-(const FTextLocation& Other) const
    {
        return FTextLocation(LineIndex - Other.LineIndex, CharIndex - Other.CharIndex);
    }
};

// Text selection (start + end locations)
struct FTextSelection
{
    FTextLocation Start;
    FTextLocation End;
    
    FTextSelection() = default;
    FTextSelection(const FTextLocation& InStart, const FTextLocation& InEnd)
        : Start(InStart), End(InEnd) {}
    
    bool IsValid() const { return Start != End; }
    bool IsEmpty() const { return Start == End; }
    
    // Get normalized selection (Start < End)
    void Normalize()
    {
        if (Start.LineIndex > End.LineIndex ||
            (Start.LineIndex == End.LineIndex && Start.CharIndex > End.CharIndex))
        {
            std::swap(Start, End);
        }
    }
};

// Text line info for layout
class FTextLineInfo
{
public:
    std::string Text;
    float Height {0.0f};
    float Baseline {0.0f};  // Y position of baseline relative to line top
    
    FTextLineInfo() = default;
    explicit FTextLineInfo(std::string InText) : Text(std::move(InText)) {}
};

// ============================================================================
// SMultiLineEditableText - Multi-line text editor (UE SMultiLineEditableText analogue)
// ============================================================================

class SMultiLineEditableText : public SWidget
{
public:
    // Configuration options
    struct FTextOptions
    {
        float FontSize {14.0f};
        float LineSpacing {1.2f};
        float MinWidth {100.0f};
        float MinHeight {60.0f};
        FMargin Padding {6.0f, 6.0f};
        
        UIColor BackgroundColor {0.08f, 0.08f, 0.10f, 1.0f};
        UIColor BorderColor {0.35f, 0.36f, 0.40f, 1.0f};
        UIColor FocusBorderColor {0.36f, 0.62f, 0.96f, 1.0f};
        UIColor TextColor {0.92f, 0.93f, 0.96f, 1.0f};
        UIColor HintColor {0.45f, 0.46f, 0.52f, 1.0f};
        UIColor SelectionColor {0.25f, 0.50f, 0.90f, 0.40f};
        UIColor CursorColor {1.0f, 1.0f, 1.0f, 1.0f};
        UIColor ScrollBarColor {0.40f, 0.40f, 0.46f, 0.85f};
        UIColor CompositionColor {0.25f, 0.75f, 0.40f, 0.40f};
        
        bool AutoWrap {true};           // Auto-wrap long lines
        bool ReadOnly {false};          // Read-only mode
        bool IsPassword {false};        // Password mode (display *** instead of text)
        bool ShowScrollBar {true};      // Show scroll bar
    };

    SMultiLineEditableText() = default;
    explicit SMultiLineEditableText(const FTextOptions& InOptions) : Options(InOptions) {}

    // Set text content
    void SetText(const std::string& InText);
    const std::string& GetText() const { return TextContent; }
    
    // Hint text (placeholder)
    void SetHintText(const std::string& InHint) { HintText = InHint; }
    const std::string& GetHintText() const { return HintText; }
    
    // Selection management
    void SetSelection(const FTextLocation& Start, const FTextLocation& End);
    void SelectAll();
    void ClearSelection() { Selection = FTextSelection(); }
    const FTextSelection& GetSelection() const { return Selection; }
    
    // Cursor management
    void SetCursorLocation(const FTextLocation& Location);
    FTextLocation GetCursorLocation() const { return CursorLocation; }
    
    // Scroll management
    void ScrollToCursor();
    void ScrollToLine(int32_t LineIndex);
    
    // Text modification
    void InsertTextAtCursor(const std::string& Text);
    void DeleteSelection();
    void ReplaceSelection(const std::string& Text);
    
    // Clipboard operations
    void CopySelection();
    void CutSelection();
    void PasteText();
    
    // Children interface
    int GetChildCount() const override { return 0; }  // Leaf widget
    std::shared_ptr<SWidget> GetChildAt(int /*index*/) const override { return nullptr; }

    // Layout
    Vector2 ComputeDesiredSize() const override;
    void ArrangeChildren(const FGeometry& /*allotted*/, std::vector<FArrangedWidget>& /*out*/) const override {}

    // Painting
    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override;

    // Input handling
    void OnMouseEnter() override;
    void OnMouseLeave() override;
    void OnMouseMove(const Vector2& pos) override;
    FReply OnMouseButtonDown(const Vector2& pos, int button) override;
    FReply OnMouseButtonUp(const Vector2& pos, int button) override;
    void OnMouseCaptureLost() override;
    FReply OnMouseWheel(const Vector2& pos, float delta) override;
    
    bool SupportsKeyboardFocus() const override { return !Options.ReadOnly; }
    void OnFocusReceived() override;
    void OnFocusLost() override;
    void OnKeyChar(unsigned int codepoint) override;
    FReply OnKeyDown(EKey key) override;
    ECursorType GetCursor() const override;

    // Clipping
    bool ClipsContent() const override { return true; }

private:
    // Text utilities
    std::vector<FTextLineInfo> SplitIntoLines(const std::string& Text) const;
    std::string JoinLines(const std::vector<FTextLineInfo>& Lines) const;
    
    // Layout calculations
    void ClampScrollOffset();
    float GetMaxScrollOffset() const;
    float GetScrollBarThumbSize() const;
    float GetScrollBarThumbPosition() const;
    
    // UTF-8 utilities
    size_t GetPrevCharStart(size_t pos) const;
    size_t GetNextCharStart(size_t pos) const;
    int32_t Utf8ToCharIndex(size_t BytePos, const std::string& Line) const;
    size_t CharIndexToUtf8(int32_t CharIndex, const std::string& Line) const;
    
    // Line info
    const FTextLineInfo& GetLineInfo(int32_t LineIndex) const;
    int32_t GetNumLines() const { return static_cast<int32_t>(Lines.size()); }
    
    // Layout calculations
    float GetLineHeight(int32_t LineIndex) const;
    float GetLineTopY(int32_t LineIndex) const;
    float GetLineBottomY(int32_t LineIndex) const;
    float GetContentHeight() const;
    
    // Hit testing
    FTextLocation HitTestLocation(const Vector2& LocalPos) const;
    
    // Selection utilities
    std::string GetSelectedText() const;
    void DeleteRange(const FTextLocation& Start, const FTextLocation& End);
    
    // Cursor utilities
    void MoveCursorHome(bool bCtrl = false);
    void MoveCursorEnd(bool bCtrl = false);
    void MoveCursorLeft(bool bShift = false, bool bCtrl = false);
    void MoveCursorRight(bool bShift = false, bool bCtrl = false);
    void MoveCursorUp(bool bShift = false);
    void MoveCursorDown(bool bShift = false);
    
    // Drawing helpers
    void DrawBackground(const FPaintContext& ctx, const FGeometry& geom) const;
    void DrawSelection(const FPaintContext& ctx, const FGeometry& geom) const;
    void DrawComposition(const FPaintContext& ctx, const FGeometry& geom) const;
    void DrawText(const FPaintContext& ctx, const FGeometry& geom) const;
    void DrawCursor(const FPaintContext& ctx, const FGeometry& geom) const;
    void DrawScrollBar(const FPaintContext& ctx, const FGeometry& geom) const;
    
    // Scrollbar
    void ApplyThumbDrag(float PointerY, const UIRect& Rect);

public:
    FTextOptions Options;
    
private:
    
    std::string TextContent;
    std::string HintText;
    std::vector<FTextLineInfo> Lines;
    
    FTextLocation CursorLocation;
    FTextSelection Selection;
    
    float ScrollOffsetY {0.0f};
    float ScrollOffsetX {0.0f};
    
    bool m_HasFocus {false};
    bool m_DraggingThumb {false};
    // True between a press in the text area and its release. OnMouseMove now
    // arrives on every frame the cursor is over the widget, not only while it
    // holds the capture, so the move handler needs this to tell a real
    // drag-selection apart from the cursor simply resting over focused text.
    bool m_SelectingWithMouse {false};
    float m_DragGrab {0.0f};
    
    // Composition (IME) support
    std::string CompositionText;
    int32_t CompositionCursorPos {0};
    
    // Blinking cursor
    mutable float m_CursorBlinkTime {0.0f};
    mutable bool m_CursorVisible {true};
    
    ISlateTextMeasurer* m_TextMeasurer {nullptr};

    // UTF-8 helper
    static void AppendUtf8(std::string& s, unsigned int cp);

public:
    // Callbacks
    std::function<void(const std::string&)> OnTextChanged;
};

// ============================================================================
// STextBlock - Single line text display (UE STextBlock analogue)
// NOTE: This inline definition only used when the standalone STextBlock.h
//       is NOT included (to avoid redefinition conflicts).
// ============================================================================

#ifndef ZSLATE_STEXTBLOCK_DEFINED
#define ZSLATE_STEXTBLOCK_DEFINED

class STextBlock : public SLeafWidget
{
public:
    std::string Text;
    float FontSize {14.0f};
    UIColor Color {1.0f, 1.0f, 1.0f, 1.0f};
    TextAnchor Alignment {TextAnchor::MiddleLeft};

    void SetText(std::string text) { Text = std::move(text); }

    Vector2 ComputeDesiredSize() const override
    {
        if (m_TextMeasurer)
            return m_TextMeasurer->Measure(Text, FontSize);
        // Coarse fallback before a measurer is installed.
        return Vector2(static_cast<float>(Text.size()) * FontSize * 0.5f, FontSize * 1.2f);
    }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (ctx.Renderer == nullptr || Text.empty())
            return;
        ctx.Renderer->DrawTextLabel(geom.ToRect(), Text, FontSize, Color, Alignment, TextWrapMode::NoWrap, nullptr);
    }

    ISlateTextMeasurer* m_TextMeasurer {nullptr};
};

#endif // ZSLATE_STEXTBLOCK_DEFINED

// ============================================================================
// SEditableTextBox - Single line text input (UE SEditableText analogue)
// NOTE: This inline definition only used when the standalone SEditableTextBox.h
//       is NOT included (to avoid redefinition conflicts).
// ============================================================================

#ifndef ZSLATE_SEDITABLETEXTBOX_DEFINED
#define ZSLATE_SEDITABLETEXTBOX_DEFINED

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
    UIColor SelectionColor {0.25f, 0.50f, 0.90f, 0.50f};

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
        if (m_TextMeasurer && !Text.empty())
            text_w = m_TextMeasurer->Measure(Text, FontSize).x;
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
        ctx.Renderer->DrawQuad(rect, BackgroundColor);
        ctx.Renderer->DrawRect(rect, m_Focused ? FocusBorderColor : BorderColor, 1.0f);

        const UIRect text_rect(rect.x + Padding.Left,
                               rect.y + Padding.Top,
                               rect.w - Padding.GetTotalHorizontal(),
                               rect.h - Padding.GetTotalVertical());

        // Draw selection highlight
        if (HasSelection() && ctx.Renderer != nullptr)
        {
            size_t sel_start, sel_end;
            GetSelectionRange(sel_start, sel_end);

            std::string text_before_sel = Text.substr(0, sel_start);
            std::string sel_text = Text.substr(sel_start, sel_end - sel_start);

            float sel_x = text_rect.x;
            if (!text_before_sel.empty() && m_TextMeasurer)
                sel_x += m_TextMeasurer->Measure(text_before_sel, FontSize).x;

            float sel_w = FontSize;
            if (!sel_text.empty() && m_TextMeasurer)
                sel_w = m_TextMeasurer->Measure(sel_text, FontSize).x;

            ctx.Renderer->DrawQuad(
                UIRect(sel_x, text_rect.y, std::max(sel_w, 1.0f), text_rect.h),
                SelectionColor);
        }

        // Draw text
        if (Text.empty() && !m_Focused && !HintText.empty())
        {
            ctx.Renderer->DrawTextLabel(text_rect, HintText, FontSize, HintColor, TextAnchor::MiddleLeft,
                                   TextWrapMode::NoWrap, nullptr);
        }
        else if (!Text.empty())
        {
            ctx.Renderer->DrawTextLabel(text_rect, Text, FontSize, TextColor, TextAnchor::MiddleLeft,
                                   TextWrapMode::NoWrap, nullptr);
        }

        // Draw caret
        if (m_Focused)
        {
            float caret_x = text_rect.x;
            if (!Text.empty() && m_CaretPos > 0 && m_TextMeasurer)
            {
                std::string text_before_caret = Text.substr(0, m_CaretPos);
                caret_x += m_TextMeasurer->Measure(text_before_caret, FontSize).x;
            }
            const float caret_h = FontSize;
            const float caret_y = text_rect.y + (text_rect.h - caret_h) * 0.5f;
            ctx.Renderer->DrawQuad(UIRect(caret_x + 1.0f, caret_y, 1.5f, caret_h), TextColor);
        }
    }

    bool SupportsKeyboardFocus() const override { return true; }
    void OnFocusReceived() override { m_Focused = true; }
    void OnFocusLost() override
    {
        m_Focused = false;
        m_SelectionStart = std::string::npos;
    }

    FReply OnMouseButtonDown(const Vector2& pos, int button) override
    {
        if (button != 0)
            return FReply::Unhandled();

        m_Focused = true;
        m_Dragging = true;

        if (!Text.empty())
            m_CaretPos = Text.length();
        else
            m_CaretPos = 0;

        m_SelectionStart = m_CaretPos;

        return FReply::Handled();
    }

    FReply OnMouseButtonUp(const Vector2& pos, int button) override
    {
        if (button != 0)
            return FReply::Unhandled();

        m_Dragging = false;

        if (m_SelectionStart != std::string::npos && m_SelectionStart == m_CaretPos)
            m_SelectionStart = std::string::npos;

        return FReply::Handled();
    }

    void OnKeyChar(unsigned int codepoint) override
    {
        if (codepoint >= 1 && codepoint <= 26)
            return;
        if (codepoint == 127)
            return;
        if (codepoint < 32)
            return;

        DeleteSelectedText();
        std::string utf8_char;
        AppendUtf8(utf8_char, codepoint);
        Text.insert(m_CaretPos, utf8_char);
        m_CaretPos += utf8_char.length();
        m_SelectionStart = std::string::npos;

        if (OnTextChanged)
            OnTextChanged(Text);
    }

    FReply OnKeyDown(EKey key) override
    {
        if (!m_Focused)
            return FReply::Unhandled();

        if (SlateInputRouter::IsCtrlDown())
        {
            switch (key)
            {
                case EKey::A: SelectAll(); return FReply::Handled();
                case EKey::C: CopyToClipboard(); return FReply::Handled();
                case EKey::V: PasteFromClipboard(); return FReply::Handled();
                case EKey::X: CutToClipboard(); return FReply::Handled();
                default: break;
            }
        }

        switch (key)
        {
            case EKey::Backspace:
                if (HasSelection()) { DeleteSelectedText(); }
                else if (!Text.empty() && m_CaretPos > 0)
                {
                    size_t char_start = GetPreviousCharStart(m_CaretPos);
                    Text.erase(char_start, m_CaretPos - char_start);
                    m_CaretPos = char_start;
                    m_SelectionStart = std::string::npos;
                    if (OnTextChanged) OnTextChanged(Text);
                }
                return FReply::Handled();

            case EKey::Delete:
                if (HasSelection()) { DeleteSelectedText(); }
                else if (!Text.empty() && m_CaretPos < Text.length())
                {
                    size_t char_end = GetNextCharStart(m_CaretPos);
                    Text.erase(m_CaretPos, char_end - m_CaretPos);
                    m_SelectionStart = std::string::npos;
                    if (OnTextChanged) OnTextChanged(Text);
                }
                return FReply::Handled();

            case EKey::Enter:
                if (OnTextCommitted) OnTextCommitted(Text);
                return FReply::Handled();

            case EKey::Left:
                if (SlateInputRouter::IsShiftDown())
                    if (m_SelectionStart == std::string::npos) m_SelectionStart = m_CaretPos;
                else
                    m_SelectionStart = std::string::npos;
                if (m_CaretPos > 0) m_CaretPos = GetPreviousCharStart(m_CaretPos);
                return FReply::Handled();

            case EKey::Right:
                if (SlateInputRouter::IsShiftDown())
                    if (m_SelectionStart == std::string::npos) m_SelectionStart = m_CaretPos;
                else
                    m_SelectionStart = std::string::npos;
                if (m_CaretPos < Text.length()) m_CaretPos = GetNextCharStart(m_CaretPos);
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
        if (!HasSelection()) return "";
        size_t start, end;
        GetSelectionRange(start, end);
        return Text.substr(start, end - start);
    }

    void DeleteSelectedText()
    {
        if (!HasSelection()) return;
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
        if (selected.empty()) return;
        if (GSlateSetClipboard) GSlateSetClipboard(selected.c_str(), GSlateClipboardUserData);
    }

    void CutToClipboard()
    {
        std::string selected = GetSelectedText();
        if (selected.empty()) return;
        if (GSlateSetClipboard) GSlateSetClipboard(selected.c_str(), GSlateClipboardUserData);
        DeleteSelectedText();
        if (OnTextChanged) OnTextChanged(Text);
    }

    void PasteFromClipboard()
    {
        if (!GSlateGetClipboard) return;
        const char* clipboard_text = GSlateGetClipboard(GSlateClipboardUserData);
        if (clipboard_text != nullptr && *clipboard_text != '\0')
        {
            DeleteSelectedText();
            std::string paste_text(clipboard_text);
            Text.insert(m_CaretPos, paste_text);
            m_CaretPos += paste_text.length();
            m_SelectionStart = std::string::npos;
            if (OnTextChanged) OnTextChanged(Text);
        }
    }

    static void AppendUtf8(std::string& s, unsigned int cp);

    size_t GetPreviousCharStart(size_t pos) const
    {
        if (pos == 0) return 0;
        size_t i = pos;
        do { --i; } while (i > 0 && (static_cast<unsigned char>(Text[i]) & 0xC0) == 0x80);
        return i;
    }

    size_t GetNextCharStart(size_t pos) const
    {
        if (pos >= Text.length()) return Text.length();
        while (pos < Text.length() && (static_cast<unsigned char>(Text[pos]) & 0xC0) == 0x80)
            ++pos;
        if (pos < Text.length())
        {
            unsigned char c = static_cast<unsigned char>(Text[pos]);
            if (c < 0x80) return pos + 1;
            else if ((c & 0xE0) == 0xC0) return pos + 2;
            else if ((c & 0xF0) == 0xE0) return pos + 3;
            else if ((c & 0xF8) == 0xF0) return pos + 4;
        }
        return Text.length();
    }

    bool m_Focused {false};
    size_t m_CaretPos;
    size_t m_SelectionStart;
    bool m_Dragging {false};
    ISlateTextMeasurer* m_TextMeasurer {nullptr};
};

#endif // ZSLATE_SEDITABLETEXTBOX_DEFINED

}  // namespace ZSlate
