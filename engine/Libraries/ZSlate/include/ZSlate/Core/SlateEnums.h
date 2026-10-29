#pragma once

#include "ZSlate/Core/ZSlateTypes.h"

// ZSlate: a retained-mode (UE Slate-inspired) UI framework.
// The widget tree is the architecture; the rendering backend is injected
// via ISlateRenderer so ZSlate can be used standalone or with any engine.
namespace ZSlate
{
// How a widget fills its allotted slot horizontally.
enum class EHorizontalAlignment
{
    Fill,    // stretch to the full slot width
    Left,    // size to desired width, anchored left
    Center,  // size to desired width, centered
    Right,   // size to desired width, anchored right
};

// How a widget fills its allotted slot vertically.
enum class EVerticalAlignment
{
    Fill,    // stretch to the full slot height
    Top,     // size to desired height, anchored top
    Center,  // size to desired height, centered
    Bottom,  // size to desired height, anchored bottom
};

// Participation in layout / paint / hit-testing.
enum class EVisibility
{
    Visible,           // painted, hit-testable, takes layout space
    Collapsed,         // not painted, NOT hit-testable, takes NO layout space
    Hidden,            // not painted, NOT hit-testable, but STILL takes layout space
    HitTestInvisible,  // painted and takes space, but transparent to hit-testing
};

// Box panel orientation.
enum class EOrientation
{
    Horizontal,
    Vertical,
};

// How a box-panel slot consumes space along the panel's main axis.
enum class ESizeRule
{
    Auto,     // size to the child's desired size
    Stretch,  // share the remaining space, weighted by FillSize
};

// Mouse cursor a widget requests while it is the deepest hovered widget (UE
// Slate FCursorReply analogue). The editor host maps this to a platform cursor;
// this enum lives in Runtime so widgets stay free of any editor dependency.
enum class ECursorType
{
    Default,
    Hand,
    ResizeEW,
    ResizeNS,
    ResizeNWSE,
    ResizeAll,
    TextBeam,
};

// Text alignment within a rectangle
enum class TextAnchor
{
    MiddleLeft,
    MiddleCenter,
    MiddleRight,
    TopLeft,
    TopCenter,
    TopRight,
    BottomLeft,
    BottomCenter,
    BottomRight,
};

// Text wrap mode
enum class TextWrapMode
{
    NoWrap,
    Wrap,
};

}  // namespace ZSlate
