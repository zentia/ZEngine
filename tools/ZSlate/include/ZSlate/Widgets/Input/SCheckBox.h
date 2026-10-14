#pragma once

// ============================================================================
// ZSlate/Widgets/Input/SCheckBox.h
//
// Toggle box with an optional text label.
// ============================================================================

#include "../SWidget.h"
#include "../../Core/SlateTextMeasurer.h"
#include "../../Core/ZSlateTypes.h"

#include <string>
#include <functional>

namespace ZSlate
{

class SCheckBox : public SWidget
{
public:
    bool        Checked  = false;
    std::string Label;
    float       FontSize = 14.0f;

    UIColor BoxBackgroundColor { 0.08f, 0.09f, 0.11f, 1.0f };
    UIColor BoxBorderColor     { 0.35f, 0.38f, 0.45f, 1.0f };
    UIColor CheckColor         { 0.30f, 0.65f, 1.00f, 1.0f };
    UIColor TextColor          = Colors::White;

    // Fired with the new state whenever the user toggles the box.
    std::function<void(bool)> OnCheckStateChanged;

    void SetChecked(bool checked, bool notify = false);

    Vector2 ComputeDesiredSize() const override;

    void OnPaint(const FPaintContext& context, const FGeometry& geometry) const override;

    FReply OnMouseButtonUp(const Vector2& absolutePos, int button) override;
};

} // namespace ZSlate
