#include "ZSlate/Widgets/Text/SMultiLineEditableText.h"

#include "ZSlate/Core/SlatePlatform.h"

#include <algorithm>

namespace ZSlate
{

namespace
{
    constexpr uint32_t KEY_BACK   = 0x08;
    constexpr uint32_t KEY_RETURN = 0x0D;
    constexpr uint32_t KEY_HOME   = 0x24;
    constexpr uint32_t KEY_END    = 0x23;
    constexpr uint32_t KEY_LEFT   = 0x25;
    constexpr uint32_t KEY_UP     = 0x26;
    constexpr uint32_t KEY_RIGHT  = 0x27;
    constexpr uint32_t KEY_DOWN   = 0x28;
    constexpr uint32_t KEY_DELETE = 0x2E;
}

// ---------------------------------------------------------------------------
// Content
// ---------------------------------------------------------------------------

void SMultiLineEditableText::RebuildLines(const std::string& text)
{
    m_Lines.clear();
    std::string current;
    for (const char c : text)
    {
        if (c == '\r') continue;
        if (c == '\n') { m_Lines.push_back(current); current.clear(); }
        else           { current += c; }
    }
    m_Lines.push_back(current);
    m_WrapWidth = -1.0f;
}

void SMultiLineEditableText::SyncTextFromLines()
{
    m_WrapWidth = -1.0f;
    if (OnTextChanged) OnTextChanged(GetText());
}

std::string SMultiLineEditableText::GetText() const
{
    std::string result;
    for (size_t i = 0; i < m_Lines.size(); ++i)
    {
        if (i != 0) result += '\n';
        result += m_Lines[i];
    }
    return result;
}

void SMultiLineEditableText::SetText(const std::string& text)
{
    RebuildLines(text);
    m_CursorRow  = std::min(m_CursorRow, static_cast<int32_t>(m_Lines.size()) - 1);
    m_CursorRow  = std::max(0, m_CursorRow);
    m_DesiredCol = 0;
    ClampCursor();
}

void SMultiLineEditableText::ClampCursor()
{
    if (m_Lines.empty()) m_Lines.emplace_back();
    if (m_CursorRow < 0) m_CursorRow = 0;
    if (m_CursorRow >= static_cast<int32_t>(m_Lines.size()))
        m_CursorRow = static_cast<int32_t>(m_Lines.size()) - 1;

    const int32_t lineLen = static_cast<int32_t>(Utf8::Length(m_Lines[m_CursorRow]));
    if (m_CursorCol < 0)       m_CursorCol = 0;
    if (m_CursorCol > lineLen) m_CursorCol = lineLen;
}

// ---------------------------------------------------------------------------
// Visual rows
// ---------------------------------------------------------------------------

void SMultiLineEditableText::UpdateVisualRows(float width) const
{
    const float usable = std::max(24.0f, width - m_HorizontalPad * 2.0f);

    if (m_WrapWidth == usable && !m_VisualRows.empty()) return;
    m_WrapWidth = usable;

    m_VisualRows.clear();

    if (!Options.AutoWrap)
    {
        for (size_t i = 0; i < m_Lines.size(); ++i)
        {
            FVisualRow row;
            row.LineIndex = static_cast<int32_t>(i);
            row.StartByte = 0;
            row.EndByte   = m_Lines[i].size();
            m_VisualRows.push_back(row);
        }
        return;
    }

    for (size_t lineIdx = 0; lineIdx < m_Lines.size(); ++lineIdx)
    {
        const std::string& line = m_Lines[lineIdx];
        size_t start = 0;

        while (start <= line.size())
        {
            // Greedy: extend byte by byte (code-point aligned) until the row
            // would overflow, then back off to the last break opportunity.
            size_t end = start;
            size_t lastBreak = std::string::npos;
            float  rowWidth = 0.0f;

            while (end < line.size())
            {
                uint32_t cp = 0;
                size_t   probe = end;
                const int consumed = Utf8::Decode(line, probe, cp);
                if (consumed == 0) break;

                const std::string next = line.substr(end, static_cast<size_t>(consumed));
                const float cpWidth = MeasureText(next, Options.FontSize).x;

                if (rowWidth + cpWidth > usable && end > start) break;

                rowWidth += cpWidth;
                end = probe;

                if (cp == ' ' || cp == '\t') lastBreak = end;
            }

            if (end < line.size() && lastBreak != std::string::npos && lastBreak > start)
            {
                end = lastBreak;
            }
            if (end == start && start < line.size())
            {
                // A single glyph wider than the row — emit it alone.
                uint32_t cp = 0;
                size_t   probe = start;
                const int consumed = Utf8::Decode(line, probe, cp);
                end = (consumed > 0) ? probe : line.size();
            }

            FVisualRow row;
            row.LineIndex = static_cast<int32_t>(lineIdx);
            row.StartByte = start;
            row.EndByte   = end;
            m_VisualRows.push_back(row);

            if (end <= start) break;
            start = end;
            if (start >= line.size()) break;
        }

        if (m_VisualRows.empty() || m_VisualRows.back().LineIndex != static_cast<int32_t>(lineIdx))
        {
            FVisualRow row;
            row.LineIndex = static_cast<int32_t>(lineIdx);
            row.StartByte = line.size();
            row.EndByte   = line.size();
            m_VisualRows.push_back(row);
        }
    }
}

int32_t SMultiLineEditableText::FindVisualRowIndex() const
{
    const size_t caretByte = Utf8::ByteOffsetOf(m_Lines[m_CursorRow],
                                                static_cast<size_t>(m_CursorCol));
    for (size_t i = 0; i < m_VisualRows.size(); ++i)
    {
        const FVisualRow& row = m_VisualRows[i];
        if (row.LineIndex != m_CursorRow) continue;
        if (caretByte >= row.StartByte && caretByte <= row.EndByte) return static_cast<int32_t>(i);
    }
    return static_cast<int32_t>(m_VisualRows.size()) - 1;
}

void SMultiLineEditableText::MoveCursorVertically(int deltaRows)
{
    UpdateVisualRows(GetCachedGeometry().LocalSize.x);

    int32_t index = FindVisualRowIndex() + deltaRows;
    index = std::clamp(index, 0, static_cast<int32_t>(m_VisualRows.size()) - 1);

    const FVisualRow& target = m_VisualRows[index];
    const std::string segment = m_Lines[target.LineIndex]
        .substr(target.StartByte, target.EndByte - target.StartByte);

    const int32_t segmentLen = static_cast<int32_t>(Utf8::Length(segment));
    m_CursorRow = target.LineIndex;
    m_CursorCol = static_cast<int32_t>(target.StartByte == 0 ? 0
        : Utf8::Length(m_Lines[target.LineIndex].substr(0, target.StartByte)))
        + std::min(m_DesiredCol, std::max(0, segmentLen - 1));

    ClampCursor();
    EnsureCursorVisible();
}

// ---------------------------------------------------------------------------
// Layout / paint
// ---------------------------------------------------------------------------

Vector2 SMultiLineEditableText::ComputeDesiredSize() const
{
    if (m_Lines.empty()) m_Lines.emplace_back();
    float widest = 0.0f;
    for (const std::string& line : m_Lines)
    {
        widest = std::max(widest, MeasureText(line, Options.FontSize).x);
    }
    return Vector2(0.0f, Options.MinHeight + m_HorizontalPad * 2.0f + widest * 0.0f);
}

void SMultiLineEditableText::OnPaint(const FPaintContext& context, const FGeometry& geometry) const
{
    if (!context.Renderer) return;
    if (m_Lines.empty()) m_Lines.emplace_back();

    const UIRect r = geometry.ToRect();

    context.Renderer->DrawQuad(r, BackgroundColor);
    context.Renderer->DrawRect(r, HasKeyboardFocus() ? FocusedBorderColor : BorderColor, 1.0f);

    const UIRect viewRect = r.InsetBy(1.0f);
    const float  lineH    = LineHeight();

    UpdateVisualRows(r.w);

    const float contentH   = lineH * static_cast<float>(m_VisualRows.size());
    const float maxScroll  = std::max(0.0f, contentH - (viewRect.h - m_HorizontalPad));
    m_ScrollOffset = std::clamp(m_ScrollOffset, 0.0f, maxScroll);

    context.Renderer->PushClipRect(viewRect);

    const int32_t first = std::max(0, static_cast<int32_t>(m_ScrollOffset / lineH) - 1);
    const int32_t last  = std::min(static_cast<int32_t>(m_VisualRows.size()),
                                   first + static_cast<int32_t>(viewRect.h / lineH) + 3);

    for (int32_t i = first; i < last; ++i)
    {
        const FVisualRow& row  = m_VisualRows[i];
        const std::string text = m_Lines[row.LineIndex]
            .substr(row.StartByte, row.EndByte - row.StartByte);

        const float y = viewRect.y + m_HorizontalPad * 0.5f
                      + static_cast<float>(i) * lineH - m_ScrollOffset;

        const UIRect lineRect(viewRect.x + m_HorizontalPad, y,
                              std::max(0.0f, viewRect.w - m_HorizontalPad * 2.0f), lineH);

        if (!text.empty())
        {
            context.Renderer->DrawTextLabel(lineRect, text, Options.FontSize, TextColor,
                                            TextAnchor::MiddleLeft, TextWrapMode::NoWrap, nullptr);
        }
    }

    if (HasKeyboardFocus() && IsCaretVisible())
    {
        const int32_t rowIndex = FindVisualRowIndex();
        if (rowIndex >= 0 && rowIndex < static_cast<int32_t>(m_VisualRows.size()))
        {
            const FVisualRow& row = m_VisualRows[rowIndex];
            const size_t caretByte = Utf8::ByteOffsetOf(m_Lines[m_CursorRow],
                                                        static_cast<size_t>(m_CursorCol));
            const std::string before = m_Lines[m_CursorRow]
                .substr(row.StartByte, (caretByte > row.StartByte) ? caretByte - row.StartByte : 0);

            const float x = viewRect.x + m_HorizontalPad + MeasureText(before, Options.FontSize).x;
            const float y = viewRect.y + m_HorizontalPad * 0.5f
                          + static_cast<float>(rowIndex) * lineH - m_ScrollOffset;

            context.Renderer->DrawQuad(UIRect(x, y + 2.0f, 1.5f, lineH - 4.0f), CaretColor);
        }
    }

    context.Renderer->PopClipRect();
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

FReply SMultiLineEditableText::OnMouseButtonDown(const Vector2& absolutePos, int button)
{
    if (button != 0) return FReply::Unhandled();

    const UIRect r     = GetCachedGeometry().ToRect();
    const UIRect view  = r.InsetBy(1.0f);
    const float  lineH = LineHeight();

    UpdateVisualRows(r.w);

    const float localY = absolutePos.y - view.y - m_HorizontalPad * 0.5f + m_ScrollOffset;
    int32_t rowIndex = static_cast<int32_t>(localY / lineH);
    rowIndex = std::clamp(rowIndex, 0, static_cast<int32_t>(m_VisualRows.size()) - 1);

    const FVisualRow& row = m_VisualRows[rowIndex];
    const std::string segment = m_Lines[row.LineIndex]
        .substr(row.StartByte, row.EndByte - row.StartByte);

    // Walk the segment to find the code point under the cursor.
    const float localX = absolutePos.x - view.x - m_HorizontalPad;
    float  walked = 0.0f;
    size_t offset = 0;
    size_t best   = 0;
    float  bestX  = 0.0f;
    while (offset < segment.size())
    {
        uint32_t cp = 0;
        size_t   probe = offset;
        const int consumed = Utf8::Decode(segment, probe, cp);
        if (consumed == 0) break;
        const float cpWidth = MeasureText(segment.substr(offset, static_cast<size_t>(consumed)),
                                          Options.FontSize).x;
        if (walked + cpWidth * 0.5f > localX) break;
        walked += cpWidth;
        offset  = probe;
        best    = offset;
        bestX   = walked;
    }
    (void)bestX;

    m_CursorRow  = row.LineIndex;
    m_CursorCol  = static_cast<int32_t>(Utf8::Length(segment.substr(0, best)))
                 + static_cast<int32_t>(Utf8::Length(
                       m_Lines[row.LineIndex].substr(0, row.StartByte)));
    m_DesiredCol = m_CursorCol;
    ClampCursor();

    return FReply::Handled();
}

FReply SMultiLineEditableText::OnMouseWheel(const Vector2& absolutePos, float delta)
{
    (void)absolutePos;
    m_ScrollOffset -= delta * LineHeight();
    if (m_ScrollOffset < 0.0f) m_ScrollOffset = 0.0f;
    return FReply::Handled();
}

void SMultiLineEditableText::OnFocusLost()
{
    // Nothing to flush; the model is always up to date.
}

FReply SMultiLineEditableText::OnKeyChar(uint32_t codepoint)
{
    if (Options.ReadOnly) return FReply::Unhandled();
    if (codepoint < 0x20 || codepoint == 0x7F) return FReply::Unhandled();

    ClampCursor();

    std::string& line = m_Lines[m_CursorRow];
    const size_t byteOffset = Utf8::ByteOffsetOf(line, static_cast<size_t>(m_CursorCol));

    std::string encoded;
    Utf8::Append(encoded, codepoint);
    line.insert(byteOffset, encoded);

    m_CursorCol = static_cast<int32_t>(Utf8::Length(line.substr(0, byteOffset + encoded.size())));
    m_DesiredCol = m_CursorCol;

    SyncTextFromLines();
    EnsureCursorVisible();
    return FReply::Handled();
}

FReply SMultiLineEditableText::OnKeyDown(uint32_t keyCode, bool repeat)
{
    (void)repeat;
    ClampCursor();

    if (Options.ReadOnly)
    {
        // Read-only areas still scroll.
        if (keyCode == KEY_UP)   { m_ScrollOffset = std::max(0.0f, m_ScrollOffset - LineHeight()); return FReply::Handled(); }
        if (keyCode == KEY_DOWN) { m_ScrollOffset += LineHeight(); return FReply::Handled(); }
        return FReply::Unhandled();
    }

    const int32_t lineCount = static_cast<int32_t>(m_Lines.size());
    const int32_t lineLen   = static_cast<int32_t>(Utf8::Length(m_Lines[m_CursorRow]));

    switch (keyCode)
    {
    case KEY_LEFT:
        if (m_CursorCol > 0) --m_CursorCol;
        else if (m_CursorRow > 0) { --m_CursorRow; m_CursorCol = static_cast<int32_t>(Utf8::Length(m_Lines[m_CursorRow])); }
        m_DesiredCol = m_CursorCol;
        return FReply::Handled();

    case KEY_RIGHT:
        if (m_CursorCol < lineLen) ++m_CursorCol;
        else if (m_CursorRow < lineCount - 1) { ++m_CursorRow; m_CursorCol = 0; }
        m_DesiredCol = m_CursorCol;
        return FReply::Handled();

    case KEY_UP:
        MoveCursorVertically(-1);
        return FReply::Handled();

    case KEY_DOWN:
        MoveCursorVertically(1);
        return FReply::Handled();

    case KEY_HOME:
        m_CursorCol = 0;
        m_DesiredCol = 0;
        return FReply::Handled();

    case KEY_END:
        m_CursorCol = lineLen;
        m_DesiredCol = lineLen;
        return FReply::Handled();

    case KEY_RETURN:
    {
        std::string& line = m_Lines[m_CursorRow];
        const size_t byteOffset = Utf8::ByteOffsetOf(line, static_cast<size_t>(m_CursorCol));
        const std::string tail = line.substr(byteOffset);
        line.erase(byteOffset);
        m_Lines.insert(m_Lines.begin() + m_CursorRow + 1, tail);
        ++m_CursorRow;
        m_CursorCol  = 0;
        m_DesiredCol = 0;
        SyncTextFromLines();
        EnsureCursorVisible();
        return FReply::Handled();
    }

    case KEY_BACK:
    {
        if (m_CursorCol > 0)
        {
            std::string& line = m_Lines[m_CursorRow];
            const size_t start = Utf8::ByteOffsetOf(line, static_cast<size_t>(m_CursorCol - 1));
            const size_t end   = Utf8::ByteOffsetOf(line, static_cast<size_t>(m_CursorCol));
            line.erase(start, end - start);
            m_CursorCol = static_cast<int32_t>(Utf8::Length(line.substr(0, start)));
        }
        else if (m_CursorRow > 0)
        {
            const std::string current = m_Lines[m_CursorRow];
            m_CursorCol = static_cast<int32_t>(Utf8::Length(m_Lines[m_CursorRow - 1]));
            m_Lines[m_CursorRow - 1] += current;
            m_Lines.erase(m_Lines.begin() + m_CursorRow);
            --m_CursorRow;
        }
        m_DesiredCol = m_CursorCol;
        SyncTextFromLines();
        EnsureCursorVisible();
        return FReply::Handled();
    }

    case KEY_DELETE:
    {
        if (m_CursorCol < lineLen)
        {
            std::string& line = m_Lines[m_CursorRow];
            const size_t start = Utf8::ByteOffsetOf(line, static_cast<size_t>(m_CursorCol));
            const size_t end   = Utf8::ByteOffsetOf(line, static_cast<size_t>(m_CursorCol + 1));
            line.erase(start, end - start);
        }
        else if (m_CursorRow < lineCount - 1)
        {
            m_Lines[m_CursorRow] += m_Lines[m_CursorRow + 1];
            m_Lines.erase(m_Lines.begin() + m_CursorRow + 1);
        }
        m_DesiredCol = m_CursorCol;
        SyncTextFromLines();
        return FReply::Handled();
    }

    default:
        return FReply::Unhandled();
    }
}

// ---------------------------------------------------------------------------
// Scrolling
// ---------------------------------------------------------------------------

void SMultiLineEditableText::EnsureCursorVisible() const
{
    const UIRect r     = GetCachedGeometry().ToRect();
    const float  lineH = LineHeight();
    UpdateVisualRows(r.w);

    const int32_t rowIndex = FindVisualRowIndex();
    const float   viewH    = std::max(0.0f, r.h - 2.0f - m_HorizontalPad);
    const float   top      = static_cast<float>(rowIndex) * lineH;
    const float   bottom   = top + lineH;

    if (top < m_ScrollOffset)              m_ScrollOffset = top;
    if (bottom > m_ScrollOffset + viewH)   m_ScrollOffset = bottom - viewH;
    if (m_ScrollOffset < 0.0f)             m_ScrollOffset = 0.0f;
}

void SMultiLineEditableText::ScrollToCursor()
{
    EnsureCursorVisible();
}

void SMultiLineEditableText::ScrollToBottom()
{
    const UIRect r     = GetCachedGeometry().ToRect();
    const float  lineH = LineHeight();
    UpdateVisualRows(r.w);
    const float contentH = lineH * static_cast<float>(m_VisualRows.size());
    m_ScrollOffset = std::max(0.0f, contentH - (r.h - 2.0f - m_HorizontalPad));
}

} // namespace ZSlate
