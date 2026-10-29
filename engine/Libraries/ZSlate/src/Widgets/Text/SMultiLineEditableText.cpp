#include "ZSlate/Widgets/Text/SMultiLineEditableText.h"

#include <algorithm>

namespace ZSlate
{

// ============================================================================
// SMultiLineEditableText Implementation
// ============================================================================

void SMultiLineEditableText::SetText(const std::string& InText)
{
    TextContent = InText;
    Lines = SplitIntoLines(TextContent);
    
    // Clamp cursor to end of text
    CursorLocation.LineIndex = GetNumLines() > 0 ? GetNumLines() - 1 : 0;
    CursorLocation.CharIndex = static_cast<int32_t>(Lines[CursorLocation.LineIndex].Text.length());
    
    if (OnTextChanged)
        OnTextChanged(TextContent);
}

std::vector<FTextLineInfo> SMultiLineEditableText::SplitIntoLines(const std::string& Text) const
{
    std::vector<FTextLineInfo> Result;
    std::string CurrentLine;
    
    for (char c : Text)
    {
        if (c == '\n')
        {
            Result.push_back(FTextLineInfo(std::move(CurrentLine)));
            CurrentLine.clear();
        }
        else if (c == '\r')
        {
            // Skip carriage returns
        }
        else
        {
            CurrentLine += c;
        }
    }
    
    // Don't forget the last line
    if (!CurrentLine.empty() || Result.empty())
    {
        Result.push_back(FTextLineInfo(std::move(CurrentLine)));
    }
    
    return Result;
}

std::string SMultiLineEditableText::JoinLines(const std::vector<FTextLineInfo>& Lines) const
{
    std::string Result;
    for (size_t i = 0; i < Lines.size(); ++i)
    {
        if (i > 0) Result += '\n';
        Result += Lines[i].Text;
    }
    return Result;
}

void SMultiLineEditableText::SetSelection(const FTextLocation& Start, const FTextLocation& End)
{
    Selection.Start = Start;
    Selection.End = End;
    Selection.Normalize();
}

void SMultiLineEditableText::SelectAll()
{
    Selection.Start = FTextLocation(0, 0);
    Selection.End = FTextLocation(GetNumLines() - 1, static_cast<int32_t>(GetLineInfo(GetNumLines() - 1).Text.length()));
    Selection.Normalize();
}

void SMultiLineEditableText::SetCursorLocation(const FTextLocation& Location)
{
    CursorLocation = Location;
    
    // Clamp to valid range
    if (CursorLocation.LineIndex >= GetNumLines())
    {
        CursorLocation.LineIndex = GetNumLines() - 1;
    }
    if (CursorLocation.CharIndex > static_cast<int32_t>(GetLineInfo(CursorLocation.LineIndex).Text.length()))
    {
        CursorLocation.CharIndex = static_cast<int32_t>(GetLineInfo(CursorLocation.LineIndex).Text.length());
    }
}

void SMultiLineEditableText::ScrollToCursor()
{
    float CursorY = GetLineTopY(CursorLocation.LineIndex);
    float ViewHeight = m_CachedGeometry.LocalSize.y;
    
    if (CursorY < ScrollOffsetY)
        ScrollOffsetY = CursorY;
    else if (CursorY + GetLineHeight(CursorLocation.LineIndex) > ScrollOffsetY + ViewHeight)
        ScrollOffsetY = CursorY + GetLineHeight(CursorLocation.LineIndex) - ViewHeight;
    
    ClampScrollOffset();
}

void SMultiLineEditableText::ScrollToLine(int32_t LineIndex)
{
    if (LineIndex < 0 || LineIndex >= GetNumLines()) return;
    
    float LineY = GetLineTopY(LineIndex);
    float ViewHeight = m_CachedGeometry.LocalSize.y;
    
    ScrollOffsetY = LineY;
    ClampScrollOffset();
}

void SMultiLineEditableText::InsertTextAtCursor(const std::string& InText)
{
    if (Options.ReadOnly) return;
    
    DeleteSelection();
    
    // Split input into lines
    std::vector<FTextLineInfo> NewLines = SplitIntoLines(InText);
    
    // Insert at cursor position
    int32_t InsertLine = CursorLocation.LineIndex;
    int32_t InsertChar = CursorLocation.CharIndex;
    
    // Get text before and after cursor
    std::string LineText = GetLineInfo(InsertLine).Text;
    std::string BeforeCursor = LineText.substr(0, InsertChar);
    std::string AfterCursor = LineText.substr(InsertChar);
    
    // Erase old line and insert new lines
    Lines.erase(Lines.begin() + InsertLine);
    
    // Insert new lines (NewLines is already std::vector<FTextLineInfo>)
    std::vector<FTextLineInfo> NewLineInfos;
    for (size_t i = 0; i < NewLines.size(); ++i)
    {
        const std::string& NewLineText = (i == 0) ? (BeforeCursor + NewLines[i].Text) : NewLines[i].Text;
        FTextLineInfo Info(NewLineText);
        NewLineInfos.push_back(Info);
        if (i == 0) BeforeCursor.clear();  // Only first line gets before-cursor text
    }
    
    // Add remaining after-cursor text to last new line
    if (!NewLineInfos.empty())
    {
        NewLineInfos.back().Text += AfterCursor;
    }
    else
    {
        NewLineInfos.push_back(FTextLineInfo(AfterCursor));
    }
    
    // Insert new line infos
    Lines.insert(Lines.begin() + InsertLine, NewLineInfos.begin(), NewLineInfos.end());
    
    // Update cursor position
    CursorLocation.LineIndex = InsertLine;
    CursorLocation.CharIndex = static_cast<int32_t>(NewLineInfos[0].Text.length());
    
    // Update text content
    TextContent = JoinLines(Lines);
    
    // Scroll to cursor
    ScrollToCursor();
    
    if (OnTextChanged)
        OnTextChanged(TextContent);
}

void SMultiLineEditableText::DeleteSelection()
{
    if (Selection.IsEmpty()) return;
    
    DeleteRange(Selection.Start, Selection.End);
    Selection = FTextSelection();
}

void SMultiLineEditableText::ReplaceSelection(const std::string& InText)
{
    DeleteSelection();
    InsertTextAtCursor(InText);
}

void SMultiLineEditableText::CopySelection()
{
    std::string Selected = GetSelectedText();
    if (Selected.empty()) return;
    
    if (GSlateSetClipboard)
        GSlateSetClipboard(Selected.c_str(), GSlateClipboardUserData);
}

void SMultiLineEditableText::CutSelection()
{
    std::string Selected = GetSelectedText();
    if (Selected.empty()) return;
    
    CopySelection();
    DeleteSelection();
    
    if (OnTextChanged)
        OnTextChanged(TextContent);
}

void SMultiLineEditableText::PasteText()
{
    if (Options.ReadOnly) return;
    
    if (!GSlateGetClipboard) return;
    
    const char* ClipboardText = GSlateGetClipboard(GSlateClipboardUserData);
    if (ClipboardText != nullptr && *ClipboardText != '\0')
    {
        InsertTextAtCursor(ClipboardText);
        
        if (OnTextChanged)
            OnTextChanged(TextContent);
    }
}

std::string SMultiLineEditableText::GetSelectedText() const
{
    if (Selection.IsEmpty()) return "";
    
    std::string Result;
    
    // Ensure start < end
    FTextLocation Start = Selection.Start;
    FTextLocation End = Selection.End;
    if (Start.LineIndex > End.LineIndex ||
        (Start.LineIndex == End.LineIndex && Start.CharIndex > End.CharIndex))
    {
        std::swap(Start, End);
    }
    
    for (int32_t Line = Start.LineIndex; Line <= End.LineIndex; ++Line)
    {
        const std::string& LineText = GetLineInfo(Line).Text;
        
        int32_t LineStart = (Line == Start.LineIndex) ? Start.CharIndex : 0;
        int32_t LineEnd = (Line == End.LineIndex) ? End.CharIndex : static_cast<int32_t>(LineText.length());
        
        if (Line == Start.LineIndex && Line == End.LineIndex)
        {
            Result += LineText.substr(LineStart, LineEnd - LineStart);
        }
        else if (Line == Start.LineIndex)
        {
            Result += LineText.substr(LineStart);
        }
        else if (Line == End.LineIndex)
        {
            Result += LineText.substr(0, LineEnd);
        }
        else
        {
            Result += LineText;
        }
        
        if (Line < End.LineIndex)
            Result += '\n';
    }
    
    return Result;
}

void SMultiLineEditableText::DeleteRange(const FTextLocation& Start, const FTextLocation& End)
{
    if (Start == End) return;
    
    // Ensure start < end
    FTextLocation A = Start;
    FTextLocation B = End;
    if (A.LineIndex > B.LineIndex || (A.LineIndex == B.LineIndex && A.CharIndex > B.CharIndex))
        std::swap(A, B);
    
    // If same line, simple erase
    if (A.LineIndex == B.LineIndex)
    {
        std::string& LineText = Lines[A.LineIndex].Text;
        LineText.erase(A.CharIndex, B.CharIndex - A.CharIndex);
        return;
    }
    
    // Different lines - merge and erase
    // Get text before A and after B
    std::string BeforeText = Lines[A.LineIndex].Text.substr(0, A.CharIndex);
    std::string AfterText = Lines[B.LineIndex].Text.substr(B.CharIndex);
    
    // Remove lines from A.LineIndex to B.LineIndex
    Lines.erase(Lines.begin() + A.LineIndex, Lines.begin() + B.LineIndex + 1);
    
    // Insert merged line
    FTextLineInfo NewLine(BeforeText + AfterText);
    Lines.insert(Lines.begin() + A.LineIndex, NewLine);
    
    // Update text content
    TextContent = JoinLines(Lines);
}

void SMultiLineEditableText::MoveCursorHome(bool bCtrl)
{
    if (bCtrl)
    {
        CursorLocation.LineIndex = 0;
        CursorLocation.CharIndex = 0;
    }
    else
    {
        CursorLocation.CharIndex = 0;
    }
}

void SMultiLineEditableText::MoveCursorEnd(bool bCtrl)
{
    if (bCtrl)
    {
        CursorLocation.LineIndex = GetNumLines() - 1;
        CursorLocation.CharIndex = static_cast<int32_t>(GetLineInfo(GetNumLines() - 1).Text.length());
    }
    else
    {
        CursorLocation.CharIndex = static_cast<int32_t>(GetLineInfo(CursorLocation.LineIndex).Text.length());
    }
}

void SMultiLineEditableText::MoveCursorLeft(bool bShift, bool bCtrl)
{
    if (bCtrl)
    {
        // Move to start of line
        CursorLocation.CharIndex = 0;
        if (SlateInputRouter::IsShiftDown())
            Selection.End = CursorLocation;
        else
            Selection = FTextSelection();
        return;
    }
    
    if (CursorLocation.CharIndex > 0)
    {
        CursorLocation.CharIndex--;
    }
    else if (CursorLocation.LineIndex > 0)
    {
        CursorLocation.LineIndex--;
        CursorLocation.CharIndex = static_cast<int32_t>(GetLineInfo(CursorLocation.LineIndex).Text.length());
    }
    
    if (SlateInputRouter::IsShiftDown())
        Selection.End = CursorLocation;
    else
        Selection = FTextSelection();
}

void SMultiLineEditableText::MoveCursorRight(bool bShift, bool bCtrl)
{
    if (bCtrl)
    {
        // Move to end of line
        CursorLocation.CharIndex = static_cast<int32_t>(GetLineInfo(CursorLocation.LineIndex).Text.length());
        if (SlateInputRouter::IsShiftDown())
            Selection.End = CursorLocation;
        else
            Selection = FTextSelection();
        return;
    }
    
    if (CursorLocation.CharIndex < static_cast<int32_t>(GetLineInfo(CursorLocation.LineIndex).Text.length()))
    {
        CursorLocation.CharIndex++;
    }
    else if (CursorLocation.LineIndex < GetNumLines() - 1)
    {
        CursorLocation.LineIndex++;
        CursorLocation.CharIndex = 0;
    }
    
    if (SlateInputRouter::IsShiftDown())
        Selection.End = CursorLocation;
    else
        Selection = FTextSelection();
}

void SMultiLineEditableText::MoveCursorUp(bool bShift)
{
    if (CursorLocation.LineIndex > 0)
    {
        CursorLocation.LineIndex--;
        
        // Try to keep same column position
        int32_t NewLineLen = static_cast<int32_t>(GetLineInfo(CursorLocation.LineIndex).Text.length());
        if (CursorLocation.CharIndex > NewLineLen)
        {
            CursorLocation.CharIndex = NewLineLen;
        }
    }
    
    if (SlateInputRouter::IsShiftDown())
        Selection.End = CursorLocation;
    else
        Selection = FTextSelection();
}

void SMultiLineEditableText::MoveCursorDown(bool bShift)
{
    if (CursorLocation.LineIndex < GetNumLines() - 1)
    {
        CursorLocation.LineIndex++;
        
        // Try to keep same column position
        int32_t NewLineLen = static_cast<int32_t>(GetLineInfo(CursorLocation.LineIndex).Text.length());
        if (CursorLocation.CharIndex > NewLineLen)
        {
            CursorLocation.CharIndex = NewLineLen;
        }
    }
    
    if (SlateInputRouter::IsShiftDown())
        Selection.End = CursorLocation;
    else
        Selection = FTextSelection();
}

// ============================================================================
// Layout and Drawing
// ============================================================================

const FTextLineInfo& SMultiLineEditableText::GetLineInfo(int32_t LineIndex) const
{
    static FTextLineInfo EmptyLine("");
    if (LineIndex < 0 || LineIndex >= static_cast<int32_t>(Lines.size()))
        return EmptyLine;
    return Lines[static_cast<size_t>(LineIndex)];
}

float SMultiLineEditableText::GetLineHeight(int32_t LineIndex) const
{
    return Options.FontSize * Options.LineSpacing;
}

float SMultiLineEditableText::GetLineTopY(int32_t LineIndex) const
{
    float Y = 0.0f;
    for (int32_t i = 0; i < LineIndex; ++i)
    {
        Y += GetLineHeight(i);
    }
    return Y;
}

float SMultiLineEditableText::GetLineBottomY(int32_t LineIndex) const
{
    return GetLineTopY(LineIndex) + GetLineHeight(LineIndex);
}

float SMultiLineEditableText::GetContentHeight() const
{
    float Height = 0.0f;
    for (int32_t i = 0; i < GetNumLines(); ++i)
    {
        Height += GetLineHeight(i);
    }
    return Height;
}

FTextLocation SMultiLineEditableText::HitTestLocation(const Vector2& LocalPos) const
{
    float ViewTop = ScrollOffsetY;
    
    for (int32_t Line = 0; Line < GetNumLines(); ++Line)
    {
        float LineTop = GetLineTopY(Line);
        float LineBottom = GetLineBottomY(Line);
        
        if (LocalPos.y >= LineTop - ViewTop && LocalPos.y < LineBottom - ViewTop)
        {
            // Found the line, now find the character
            const std::string& LineText = GetLineInfo(Line).Text;
            
            int32_t CharIndex = 0;
            float X = 0.0f;
            
            for (size_t i = 0; i < LineText.length(); ++i)
            {
                // Count UTF-8 character
                size_t CharLen = 1;
                unsigned char c = static_cast<unsigned char>(LineText[i]);
                if (c < 0x80) CharLen = 1;
                else if ((c & 0xE0) == 0xC0) CharLen = 2;
                else if ((c & 0xF0) == 0xE0) CharLen = 3;
                else if ((c & 0xF8) == 0xF0) CharLen = 4;
                
                std::string CharStr = LineText.substr(i, CharLen);
                float CharWidth = m_TextMeasurer ? m_TextMeasurer->Measure(CharStr, Options.FontSize).x : Options.FontSize * 0.6f;
                
                float CharRight = X + CharWidth;
                
                // Check if click is in this character
                if (LocalPos.x >= X && LocalPos.x < CharRight)
                {
                    // Determine if closer to left or right edge
                    if (LocalPos.x < X + CharWidth * 0.5f)
                        CharIndex = static_cast<int32_t>(i / CharLen);  // UTF-8 safe: count full chars
                    else
                        CharIndex = static_cast<int32_t>(i / CharLen) + 1;
                    break;
                }
                
                X = CharRight;
                i += CharLen - 1;  // Skip remaining UTF-8 bytes
            }
            
            return FTextLocation(Line, CharIndex);
        }
    }
    
    // If past last line, return end of last line
    if (GetNumLines() > 0)
    {
        int32_t LastLineLen = static_cast<int32_t>(GetLineInfo(GetNumLines() - 1).Text.length());
        return FTextLocation(GetNumLines() - 1, LastLineLen);
    }
    
    return FTextLocation(0, 0);
}

void SMultiLineEditableText::ClampScrollOffset()
{
    float MaxScrollY = GetMaxScrollOffset();
    if (ScrollOffsetY < 0.0f) ScrollOffsetY = 0.0f;
    if (ScrollOffsetY > MaxScrollY) ScrollOffsetY = MaxScrollY;
}

float SMultiLineEditableText::GetMaxScrollOffset() const
{
    float ContentHeight = GetContentHeight();
    float ViewHeight = m_CachedGeometry.LocalSize.y;
    return (ContentHeight > ViewHeight) ? (ContentHeight - ViewHeight) : 0.0f;
}

Vector2 SMultiLineEditableText::ComputeDesiredSize() const
{
    float Width = m_CachedGeometry.LocalSize.x > 0.0f ? m_CachedGeometry.LocalSize.x : Options.MinWidth;
    float Height = GetContentHeight();

    return Vector2(std::max(Width, Options.MinWidth), std::max(Height, Options.MinHeight));
}

void SMultiLineEditableText::OnPaint(const FPaintContext& ctx, const FGeometry& geom) const
{
    if (ctx.Renderer == nullptr)
        return;
    
    DrawBackground(ctx, geom);
    DrawSelection(ctx, geom);
    DrawComposition(ctx, geom);
    DrawText(ctx, geom);
    DrawCursor(ctx, geom);
    
    if (Options.ShowScrollBar && GetContentHeight() > geom.LocalSize.y)
    {
        DrawScrollBar(ctx, geom);
    }
}

void SMultiLineEditableText::DrawBackground(const FPaintContext& ctx, const FGeometry& geom) const
{
    const UIRect Rect = geom.ToRect();
    ctx.Renderer->DrawQuad(Rect, Options.BackgroundColor);
    ctx.Renderer->DrawRect(Rect, m_HasFocus ? Options.FocusBorderColor : Options.BorderColor, 1.0f);
}

void SMultiLineEditableText::DrawSelection(const FPaintContext& ctx, const FGeometry& geom) const
{
    if (Selection.IsEmpty()) return;
    
    // Normalize selection
    FTextLocation Start = Selection.Start;
    FTextLocation End = Selection.End;
    if (Start.LineIndex > End.LineIndex || (Start.LineIndex == End.LineIndex && Start.CharIndex > End.CharIndex))
        std::swap(Start, End);
    
    const UIRect Rect = geom.ToRect();
    const float ViewTop = ScrollOffsetY;
    const float ViewBottom = ViewTop + Rect.h;
    
    for (int32_t Line = Start.LineIndex; Line <= End.LineIndex; ++Line)
    {
        float LineTop = GetLineTopY(Line);
        float LineBottom = GetLineBottomY(Line);
        
        // Skip if line is not visible
        if (LineBottom <= ViewTop || LineTop >= ViewBottom)
            continue;
        
        // Calculate selection rectangle
        float SelLeft = Rect.x + Options.Padding.Left;
        float SelRight = Rect.x + Rect.w - Options.Padding.Right;
        float SelTop = Rect.y + LineTop - ViewTop + Options.Padding.Top;
        float SelBottom = Rect.y + LineBottom - ViewTop - Options.Padding.Bottom;
        
        if (Line == Start.LineIndex && Line == End.LineIndex)
        {
            // Partial selection within same line
            // TODO: Calculate exact character positions
        }
        else if (Line == Start.LineIndex)
        {
            // From start character to end of line
            // TODO: Calculate exact position
        }
        else if (Line == End.LineIndex)
        {
            // From start of line to end character
            // TODO: Calculate exact position
        }
        
        // Clamp to visible area
        if (SelTop < Rect.y) SelTop = Rect.y;
        if (SelBottom > Rect.y + Rect.h) SelBottom = Rect.y + Rect.h;
        
        ctx.Renderer->DrawQuad(UIRect(SelLeft, SelTop, SelRight - SelLeft, SelBottom - SelTop), Options.SelectionColor);
    }
}

void SMultiLineEditableText::DrawComposition(const FPaintContext& ctx, const FGeometry& geom) const
{
    if (CompositionText.empty()) return;
    
    // Draw composition text with special color
    // TODO: Implement composition drawing
}

void SMultiLineEditableText::DrawText(const FPaintContext& ctx, const FGeometry& geom) const
{
    if (TextContent.empty() && !m_HasFocus && !HintText.empty())
    {
        const UIRect Rect = geom.ToRect();
        const UIRect TextRect(
            Rect.x + Options.Padding.Left,
            Rect.y + Options.Padding.Top,
            Rect.w - Options.Padding.GetTotalHorizontal(),
            Rect.h - Options.Padding.GetTotalVertical()
        );
        
        ctx.Renderer->DrawTextLabel(TextRect, HintText, Options.FontSize, Options.HintColor, 
                               TextAnchor::TopLeft, TextWrapMode::NoWrap, nullptr);
        return;
    }
    
    const UIRect Rect = geom.ToRect();
    const float ViewTop = ScrollOffsetY;
    const float ViewBottom = ViewTop + Rect.h;
    
    for (int32_t Line = 0; Line < GetNumLines(); ++Line)
    {
        float LineTop = GetLineTopY(Line);
        float LineBottom = GetLineBottomY(Line);
        
        // Skip if line is not visible
        if (LineBottom <= ViewTop || LineTop >= ViewBottom)
            continue;
        
        const std::string& LineText = GetLineInfo(Line).Text;
        
        // Clamp visible portion
        float VisibleTop = std::max(LineTop, ViewTop);
        float VisibleBottom = std::min(LineBottom, ViewBottom);
        
        UIRect TextRect(
            Rect.x + Options.Padding.Left,
            Rect.y + VisibleTop - ViewTop + Options.Padding.Top,
            Rect.w - Options.Padding.GetTotalHorizontal(),
            VisibleBottom - VisibleTop
        );
        
        // Apply password masking
        std::string DisplayText = LineText;
        if (Options.IsPassword)
        {
            DisplayText = std::string(LineText.length(), '*');
        }
        
        ctx.Renderer->DrawTextLabel(TextRect, DisplayText, Options.FontSize, Options.TextColor,
                               TextAnchor::TopLeft, TextWrapMode::NoWrap, nullptr);
    }
}

void SMultiLineEditableText::DrawCursor(const FPaintContext& ctx, const FGeometry& geom) const
{
    if (!m_HasFocus) return;
    
    // Blink cursor
    m_CursorBlinkTime += 0.016f;  // ~60 FPS
    if (m_CursorBlinkTime > 0.5f)
    {
        m_CursorBlinkTime = 0.0f;
        m_CursorVisible = !m_CursorVisible;
    }
    
    if (!m_CursorVisible) return;
    
    const UIRect Rect = geom.ToRect();
    float CursorX = Rect.x + Options.Padding.Left;
    float CursorY = Rect.y + GetLineTopY(CursorLocation.LineIndex) - ScrollOffsetY + Options.Padding.Top;
    float CursorHeight = GetLineHeight(CursorLocation.LineIndex) - Options.Padding.GetTotalVertical();
    
    // Draw vertical cursor line
    ctx.Renderer->DrawRect(UIRect(CursorX, CursorY, 2.0f, CursorHeight), Options.CursorColor, 1.0f);
}

void SMultiLineEditableText::DrawScrollBar(const FPaintContext& ctx, const FGeometry& geom) const
{
    const UIRect Rect = geom.ToRect();
    const float ViewHeight = Rect.h;
    const float ContentHeight = GetContentHeight();
    
    float ThumbSize = GetScrollBarThumbSize();
    float ThumbPos = GetScrollBarThumbPosition();
    
    if (ThumbSize <= 0.0f) return;
    
    ctx.Renderer->DrawQuad(UIRect(Rect.x + Rect.w - Options.ScrollBarColor.x - Options.ScrollBarColor.z,
                                   Rect.y + ThumbPos,
                                   Options.ScrollBarColor.x + Options.ScrollBarColor.z,
                                   ThumbSize),
                           Options.ScrollBarColor);
}

float SMultiLineEditableText::GetScrollBarThumbPosition() const
{
    float ContentHeight = GetContentHeight();
    float ViewHeight = m_CachedGeometry.LocalSize.y;
    
    if (ContentHeight <= ViewHeight) return 0.0f;
    
    float MaxScroll = ContentHeight - ViewHeight;
    float t = (MaxScroll > 0.0f) ? (ScrollOffsetY / MaxScroll) : 0.0f;
    return t * (ViewHeight - GetScrollBarThumbSize());
}

float SMultiLineEditableText::GetScrollBarThumbSize() const
{
    float ContentHeight = GetContentHeight();
    float ViewHeight = m_CachedGeometry.LocalSize.y;
    
    if (ContentHeight <= ViewHeight) return 0.0f;
    
    return ViewHeight * (ViewHeight / ContentHeight);
}

void SMultiLineEditableText::ApplyThumbDrag(float PointerY, const UIRect& Rect)
{
    float ContentHeight = GetContentHeight();
    float ViewHeight = Rect.h;
    
    if (ContentHeight <= ViewHeight) return;
    
    float ThumbSize = GetScrollBarThumbSize();
    float Track = ViewHeight - ThumbSize;
    
    if (Track <= 0.0f)
    {
        ScrollOffsetY = 0.0f;
        return;
    }
    
    float ThumbY = PointerY - m_DragGrab - Rect.y;
    if (ThumbY < 0.0f) ThumbY = 0.0f;
    if (ThumbY > Track) ThumbY = Track;
    
    ScrollOffsetY = (ThumbY / Track) * (ContentHeight - ViewHeight);
    ClampScrollOffset();
}

// ============================================================================
// Input Handling
// ============================================================================

void SMultiLineEditableText::OnMouseEnter()
{
    // Cursor changes based on state
}

void SMultiLineEditableText::OnMouseLeave()
{
    // Clear hover state
}

void SMultiLineEditableText::OnMouseMove(const Vector2& pos)
{
    if (m_DraggingThumb)
    {
        ApplyThumbDrag(pos.y, m_CachedGeometry.ToRect());
        return;
    }
    
    // Update cursor position on drag. Only while a press is actually held:
    // moves are delivered every frame the cursor is over the widget, so
    // without this the caret would follow the mouse on a plain hover.
    if (m_HasFocus && m_SelectingWithMouse)
    {
        FTextLocation NewLocation = HitTestLocation(pos - m_CachedGeometry.AbsolutePosition);
        
        if (NewLocation != CursorLocation)
        {
            if (SlateInputRouter::IsShiftDown())
            {
                Selection.End = NewLocation;
            }
            else
            {
                Selection = FTextSelection();
            }
            
            CursorLocation = NewLocation;
        }
    }
}

FReply SMultiLineEditableText::OnMouseButtonDown(const Vector2& pos, int button)
{
    if (button != 0) return FReply::Unhandled();
    
    m_HasFocus = true;
    
    const float ContentHeight = GetContentHeight();
    const UIRect Rect = m_CachedGeometry.ToRect();
    const float ViewHeight = Rect.h;
    
    if (ContentHeight > ViewHeight)
    {
        const float Band = std::max<float>(Options.ScrollBarColor.x + Options.ScrollBarColor.z, 12.0f);
        if (pos.x >= Rect.x + Rect.w - Band)
        {
            // Click on scrollbar
            float ThumbSize = GetScrollBarThumbSize();
            float ThumbPos = GetScrollBarThumbPosition();
            
            if (pos.y >= Rect.y + ThumbPos && pos.y <= Rect.y + ThumbPos + ThumbSize)
            {
                m_DragGrab = pos.y - (Rect.y + ThumbPos);
            }
            else
            {
                m_DragGrab = ThumbSize * 0.5f;
                ApplyThumbDrag(pos.y, Rect);
            }
            
            m_DraggingThumb = true;
            return FReply::Handled().CaptureMouse(this);
        }
    }
    
    // Click on text area
    FTextLocation Location = HitTestLocation(pos - m_CachedGeometry.AbsolutePosition);
    CursorLocation = Location;
    Selection = FTextSelection();
    m_SelectingWithMouse = true;

    return FReply::Handled();
}

FReply SMultiLineEditableText::OnMouseButtonUp(const Vector2& pos, int button)
{
    if (button != 0) return FReply::Unhandled();
    
    m_DraggingThumb = false;
    m_SelectingWithMouse = false;
    return FReply::Handled().ReleaseMouseCapture();
}

void SMultiLineEditableText::OnMouseCaptureLost()
{
    m_DraggingThumb = false;
    m_SelectingWithMouse = false;
}

FReply SMultiLineEditableText::OnMouseWheel(const Vector2& pos, float delta)
{
    float ScrollAmount = delta * Options.FontSize * 3.0f;
    ScrollOffsetY -= ScrollAmount;
    ClampScrollOffset();
    
    return FReply::Handled();
}

void SMultiLineEditableText::OnFocusReceived()
{
    m_HasFocus = true;
}

void SMultiLineEditableText::OnFocusLost()
{
    m_HasFocus = false;
    m_SelectingWithMouse = false;
    Selection = FTextSelection();
}

void SMultiLineEditableText::OnKeyChar(unsigned int codepoint)
{
    if (Options.ReadOnly) return;
    
    if (codepoint >= 1 && codepoint <= 26) return;  // Control characters
    if (codepoint == 127) return;  // DEL
    if (codepoint < 32) return;  // Other control characters
    
    std::string utf8_char;
    SMultiLineEditableText::AppendUtf8(utf8_char, codepoint);
    InsertTextAtCursor(utf8_char);
}

FReply SMultiLineEditableText::OnKeyDown(EKey key)
{
    if (!m_HasFocus) return FReply::Unhandled();
    if (Options.ReadOnly) return FReply::Unhandled();
    
    bool bCtrl = SlateInputRouter::IsCtrlDown();
    bool bShift = SlateInputRouter::IsShiftDown();
    
    switch (key)
    {
        case EKey::Backspace:
            if (Selection.IsValid())
            {
                DeleteSelection();
            }
            else if (CursorLocation.CharIndex > 0)
            {
                CursorLocation.CharIndex--;
                DeleteRange(CursorLocation, CursorLocation + FTextLocation(0, 1));
            }
            else if (CursorLocation.LineIndex > 0)
            {
                // Delete newline and join with previous line
                CursorLocation.LineIndex--;
                CursorLocation.CharIndex = static_cast<int32_t>(GetLineInfo(CursorLocation.LineIndex).Text.length());
                DeleteRange(CursorLocation, CursorLocation + FTextLocation(1, 0));
            }
            return FReply::Handled();
            
        case EKey::Delete:
            if (Selection.IsValid())
            {
                DeleteSelection();
            }
            else
            {
                DeleteRange(CursorLocation, CursorLocation + FTextLocation(0, 1));
            }
            return FReply::Handled();
            
        case EKey::Enter:
            InsertTextAtCursor("\n");
            return FReply::Handled();
            
        case EKey::Tab:
            InsertTextAtCursor("  ");
            return FReply::Handled();
            
        case EKey::Left:
            MoveCursorLeft(bShift, false);
            return FReply::Handled();
            
        case EKey::Right:
            MoveCursorRight(bShift, false);
            return FReply::Handled();
            
        case EKey::Up:
            MoveCursorUp(bShift);
            return FReply::Handled();
            
        case EKey::Down:
            MoveCursorDown(bShift);
            return FReply::Handled();
            
        case EKey::Home:
            MoveCursorHome(bCtrl);
            if (bShift) Selection.End = CursorLocation;
            else Selection = FTextSelection();
            return FReply::Handled();
            
        case EKey::End:
            MoveCursorEnd(bCtrl);
            if (bShift) Selection.End = CursorLocation;
            else Selection = FTextSelection();
            return FReply::Handled();
            
        default:
            break;
    }
    
    if (bCtrl)
    {
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
                
            case EKey::Z:
                // Undo (future)
                return FReply::Handled();
                
            case EKey::Y:
                // Redo (future)
                return FReply::Handled();
                
            default:
                break;
        }
    }
    
    return FReply::Unhandled();
}

ECursorType SMultiLineEditableText::GetCursor() const
{
    return ECursorType::TextBeam;  // TODO: Return IBeam when focused
}

void SMultiLineEditableText::AppendUtf8(std::string& s, unsigned int cp)
{
    if (cp < 0x80) s.push_back(static_cast<char>(cp));
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

}  // namespace ZSlate
