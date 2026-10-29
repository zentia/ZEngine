#pragma once

// Forward to canonical SSlider implementation.
// Kept for backward compatibility — prefer including Input/SSlider.h directly.
#include "ZSlate/Widgets/Input/SSlider.h"

// Backward-compat aliases for root-path includes that reference old members
namespace ZSlate
{
// SSlider::Height → use DesiredSize().y, or cast to original root member
// SSlider::HandleWidth → internal, use current knob sizing
// SSlider::MinDesiredWidth → use DesiredSize().x
// SSlider::Padding → not on SSlider; add padding via parent SBox/SBorder
}
