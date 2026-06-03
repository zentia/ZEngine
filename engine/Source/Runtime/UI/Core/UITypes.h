#pragma once

#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Core/Math/Vector2.h"
#include "Runtime/Core/Math/Vector4.h"

// UI颜色类型（RGBA）
using UIColor = Vector4;

// UI矩形
struct UIRect
{
    float x {0.0f};
    float y {0.0f};
    float width {0.0f};
    float height {0.0f};

    UIRect() = default;
    UIRect(float x_, float y_, float w_, float h_)
        : x(x_), y(y_), width(w_), height(h_) {}

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

// 文本对齐方式
enum class TextAnchor
{
    UpperLeft,
    UpperCenter,
    UpperRight,
    MiddleLeft,
    MiddleCenter,
    MiddleRight,
    LowerLeft,
    LowerCenter,
    LowerRight
};

// 文本换行模式
enum class TextWrapMode
{
    NoWrap,
    Wrap
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