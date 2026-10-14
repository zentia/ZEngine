#pragma once

// ============================================================================
// ZSlate/Widgets/Text/SMultiLineEditableText.h
//
// Multi-line text area with its own caret, selection-free editing, vertical
// scrolling and optional read-only mode.
// ============================================================================

#include "../SWidget.h"
#include "../../Core/SlateTextMeasurer.h"
#include "../../Core/ZSlateTypes.h"

#include <string>
#include <vector>
#include <functional>

namespace ZSlate
{

class SMultiLineEditableText : public SWidget
{
public:
    struct FOptions
    {
        float FontSize   = 13.0f;
        float MinHeight  = 120.0f;
        bool  AutoWrap   = true;
        bool  ReadOnly   = false;
    };

    FOptions Options;

    UIColor BackgroundColor   { 0.08f, 0.09f, 0.11f, 1.0f };
    UIColor BorderColor       { 0.25f, 0.27f, 0.32f, 1.0f };
    UIColor FocusedBorderColor{ 0.30f, 0.55f, 0.95f, 1.0f };
    UIColor TextColor         { 0.90f, 0.92f, 0.96f, 1.0f };
    UIColor CaretColor        = Colors::White;

    std::function<void(const std::string&)> OnTextChanged;

    // ---- Content -----------------------------------------------------------
    void        SetText(const std::string& text);
    std::string GetText() const;

    void ScrollToCursor();
    void ScrollToTop()    { m_ScrollOffset = 0.0f; }
    void ScrollToBottom();

    // ---- Widget overrides --------------------------------------------------
    bool WantsFillHeight() const override { return true; }

    Vector2 ComputeDesiredSize() const override;

    void OnPaint(const FPaintContext& context, const FGeometry& geometry) const override;

    FReply OnMouseButtonDown(const Vector2& absolutePos, int button) override;
    FReply OnMouseWheel(const Vector2& absolutePos, float delta) override;

    bool   IsFocusable()  const override { return !Options.ReadOnly; }
    void   OnFocusLost()        override;
    FReply OnKeyChar(uint32_t codepoint) override;
    FReply OnKeyDown(uint32_t keyCode, bool repeat) override;

private:
    // One painted row. A logical line maps to one row when AutoWrap is off and
    // to one-or-more rows when it is on.
    struct FVisualRow
    {
        int32_t LineIndex = 0;
        size_t  StartByte = 0;
        size_t  EndByte   = 0;
    };

    void RebuildLines(const std::string& text);
    void SyncTextFromLines();
    void ClampCursor();
    void UpdateVisualRows(float width) const;
    int32_t FindVisualRowIndex() const;
    void MoveCursorVertically(int deltaRows);
    void EnsureCursorVisible() const;

    float LineHeight() const { return Options.FontSize * 1.35f; }

    mutable std::vector<std::string> m_Lines;
    mutable std::vector<FVisualRow>  m_VisualRows;
    mutable float   m_WrapWidth   = -1.0f;
    mutable int32_t m_CursorRow   = 0;    // logical line index
    mutable int32_t m_CursorCol   = 0;    // code-point column inside the line
    mutable int32_t m_DesiredCol  = 0;    // remembered column for up/down
    mutable float   m_ScrollOffset = 0.0f;
    float           m_HorizontalPad = 6.0f;
};

} // namespace ZSlate
