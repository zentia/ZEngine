#pragma once

// =============================================================================
// UITypes.h — ZEngine UI type definitions (transitional)
// -----------------------------------------------------------------------------
// Phase 1: now includes ZSlateTypes.h as the canonical type source.
// Global aliases (::UIRect, ::UIColor, etc.) remain for backward compatibility
// but should be migrated to ZSlate:: types.
// =============================================================================

#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Core/Math/Vector2.h"
#include "Runtime/Core/Math/Vector4.h"
#include "ZSlate/Core/SlateGeometry.h"   // FMargin
#include "ZSlate/Core/ZSlateTypes.h"

#include <cstdint>

// ---- Legacy type aliases (migrate to ZSlate:: equivalents) ----

// UIColor: both :: and ZSlate:: are Vector4 aliases
using UIColor = Vector4;

// TextAnchor: same enum values as ZSlate::TextAnchor
using TextAnchor = ZSlate::TextAnchor;

// TextWrapMode: same values, ZSlate adds WrapAtWordBoundaryOrOverflow
using TextWrapMode = ZSlate::TextWrapMode;

// ---- Conversion helpers between ::UIRect (width/height) and ZSlate::UIRect (w/h) ----

// UI矩形 (legacy, migrate to ZSlate::UIRect)
struct UIRect
{
    float x {0.0f};
    float y {0.0f};
    float width {0.0f};
    float height {0.0f};

    UIRect() = default;
    UIRect(float x_, float y_, float w_, float h_)
        : x(x_), y(y_), width(w_), height(h_) {}

    // Convert to ZSlate::UIRect
    ZSlate::UIRect ToZSlate() const { return ZSlate::UIRect(x, y, width, height); }

    // Construct from ZSlate::UIRect
    static UIRect FromZSlate(const ZSlate::UIRect& z) { return UIRect(z.x, z.y, z.w, z.h); }

    Vector2 getMin() const { return Vector2(x, y); }
    Vector2 getMax() const { return Vector2(x + width, y + height); }
    Vector2 getCenter() const { return Vector2(x + width * 0.5f, y + height * 0.5f); }
    Vector2 getSize() const { return Vector2(width, height); }

    bool Contains(const Vector2& point) const
    {
        return point.x >= x && point.x <= x + width && point.y >= y && point.y <= y + height;
    }

    UIRect intersect(const UIRect& other) const
    {
        float minX = std::max(x, other.x);
        float minY = std::max(y, other.y);
        float maxX = std::min(x + width, other.x + other.width);
        float maxY = std::min(y + height, other.y + other.height);

        if (maxX < minX || maxY < minY)
            return UIRect(0, 0, 0, 0);

        return UIRect(minX, minY, maxX - minX, maxY - minY);
    }
};

// UI边距（用于padding、margin等）
struct FMargin
{
    float Left {0.0f};
    float Top {0.0f};
    float Right {0.0f};
    float Bottom {0.0f};

    FMargin() = default;
    explicit FMargin(float uniform)
        : Left(uniform), Top(uniform), Right(uniform), Bottom(uniform) {}
    FMargin(float horizontal, float vertical)
        : Left(horizontal), Top(vertical), Right(horizontal), Bottom(vertical) {}
    FMargin(float l, float t, float r, float b)
        : Left(l), Top(t), Right(r), Bottom(b) {}

    ZSlate::FMargin ToZSlate() const { return ZSlate::FMargin(Left, Top, Right, Bottom); }
    static FMargin FromZSlate(const ZSlate::FMargin& z) { return FMargin(z.Left, z.Top, z.Right, z.Bottom); }

    float GetTotalHorizontal() const { return Left + Right; }
    float GetTotalVertical() const { return Top + Bottom; }
};

// 矩形偏移（用于padding等）
struct RectOffset
{
    float left {0.0f};
    float right {0.0f};
    float top {0.0f};
    float bottom {0.0f};

    RectOffset() = default;
    RectOffset(float l, float r, float t, float b)
        : left(l), right(r), top(t), bottom(b) {}
    RectOffset(float all)
        : left(all), right(all), top(all), bottom(all) {}

    Vector2 getTotalSize() const { return Vector2(left + right, top + bottom); }
};

// Anchor预设
enum class AnchorPreset
{
    TopLeft,
    TopCenter,
    TopRight,
    MiddleLeft,
    MiddleCenter,
    MiddleRight,
    BottomLeft,
    BottomCenter,
    BottomRight,
    StretchTop,
    StretchMiddle,
    StretchBottom,
    StretchLeft,
    StretchCenter,
    StretchRight,
    StretchAll
};

// Canvas渲染模式
enum class CanvasRenderMode
{
    ScreenSpaceOverlay,  // 屏幕空间覆盖
    ScreenSpaceCamera,   // 屏幕空间相机
    WorldSpace           // 世界空间
};

// 输入事件类型
enum class InputEventType
{
    MouseMove,
    MouseDown,
    MouseUp,
    MouseClick,
    MouseEnter,
    MouseExit,
    KeyDown,
    KeyUp,
    Char,  // Unicode codepoint (for InputField)
    Scroll
};

// 输入事件
struct InputEvent
{
    InputEventType type;
    Vector2 mouse_position;
    int mouse_button {0};
    int key_code {0};
    unsigned int character {0};  // for InputEventType::Char
    float scroll_delta {0.0f};
};

// 按钮状态
enum class ButtonState
{
    Normal,
    Highlighted,
    Pressed,
    Disabled
};