#pragma once

// ============================================================================
// ZSlate/Widgets/Input/SEditableTextBox.h
//
// Single-line text input. `Text` is both the model and the view — read it to
// get what the user typed, write it to replace the content.
// ============================================================================

#include "../SWidget.h"
#include "../../Core/SlateTextMeasurer.h"
#include "../../Core/ZSlateTypes.h"

#include <string>
#include <functional>

namespace ZSlate
{

class SEditableTextBox : public SWidget
{
public:
    std::string Text;
    float       FontSize = 14.0f;
    float       MinWidth = 120.0f;

    UIColor BackgroundColor   { 0.08f, 0.09f, 0.11f, 1.0f };
    UIColor BorderColor       { 0.25f, 0.27f, 0.32f, 1.0f };
    UIColor FocusedBorderColor{ 0.30f, 0.55f, 0.95f, 1.0f };
    UIColor TextColor         { 0.92f, 0.94f, 0.98f, 1.0f };
    UIColor HintColor         { 0.45f, 0.48f, 0.55f, 1.0f };
    std::string HintText;

    std::function<void(const std::string&)> OnTextChanged;
    std::function<void()>                   OnEnterPressed;

    void SetText(const std::string& text);

    // ---- Widget overrides --------------------------------------------------
    Vector2 ComputeDesiredSize() const override;

    void OnPaint(const FPaintContext& context, const FGeometry& geometry) const override;

    FReply OnMouseButtonDown(const Vector2& absolutePos, int button) override;

    bool   IsFocusable() const override { return true; }
    void   OnFocusLost()       override;
    FReply OnKeyChar(uint32_t codepoint) override;
    FReply OnKeyDown(uint32_t keyCode, bool repeat) override;

private:
    void ClampCursor();

    mutable int32_t m_CaretPos = -1;   // code-point index; -1 == end of text
    mutable float   m_ScrollX  = 0.0f;
};

} // namespace ZSlate
