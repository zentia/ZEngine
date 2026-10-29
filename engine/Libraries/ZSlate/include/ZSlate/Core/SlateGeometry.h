#pragma once

#include "ZSlate/Core/SlateEnums.h"
#include "ZSlate/Core/ZSlateTypes.h"

#include <memory>
#include <string>
#include <vector>

namespace ZSlate
{
class SWidget;

// Padding / margin in pixels (Slate FMargin).
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

    float GetTotalHorizontal() const { return Left + Right; }
    float GetTotalVertical() const { return Top + Bottom; }

    bool operator==(const FMargin& Other) const
    {
        return Left == Other.Left
            && Top == Other.Top
            && Right == Other.Right
            && Bottom == Other.Bottom;
    }

    bool operator!=(const FMargin& Other) const
    {
        return !(*this == Other);
    }
};

// Resolved absolute placement of a widget for one frame (Slate FGeometry).
// Origin is screen-space top-left, pixels. ZSlate uses a flat absolute model
// for the first iteration (no per-widget render transforms yet).
struct FGeometry
{
    Vector2 AbsolutePosition {0.0f, 0.0f};
    Vector2 LocalSize {0.0f, 0.0f};

    FGeometry() = default;
    FGeometry(const Vector2& pos, const Vector2& size)
        : AbsolutePosition(pos), LocalSize(size) {}

    UIRect ToRect() const { return UIRect(AbsolutePosition.x, AbsolutePosition.y, LocalSize.x, LocalSize.y); }

    bool IsUnderLocation(const Vector2& absolute) const
    {
        return absolute.x >= AbsolutePosition.x && absolute.x <= AbsolutePosition.x + LocalSize.x &&
               absolute.y >= AbsolutePosition.y && absolute.y <= AbsolutePosition.y + LocalSize.y;
    }

    // Carve out a child geometry given an offset (relative to this widget's
    // top-left) and an explicit size.
    FGeometry MakeChild(const Vector2& local_offset, const Vector2& size) const
    {
        return FGeometry(Vector2(AbsolutePosition.x + local_offset.x, AbsolutePosition.y + local_offset.y), size);
    }
};

// A widget plus the geometry it was arranged into by its parent.
struct FArrangedWidget
{
    std::shared_ptr<SWidget> Widget;
    FGeometry Geometry;
};

// Place a child of `desired` size inside the region (x,y,w,h) according to the
// requested alignment. Fill stretches to the region; otherwise the child keeps
// its desired extent and is anchored/centered.
inline FGeometry AlignChildInRegion(float region_x,
                                    float region_y,
                                    float region_w,
                                    float region_h,
                                    const Vector2& desired,
                                    EHorizontalAlignment h_align,
                                    EVerticalAlignment v_align)
{
    float x = region_x;
    float w = region_w;
    switch (h_align)
    {
        case EHorizontalAlignment::Fill:
            w = region_w;
            break;
        case EHorizontalAlignment::Left:
            w = desired.x < region_w ? desired.x : region_w;
            x = region_x;
            break;
        case EHorizontalAlignment::Center:
            w = desired.x < region_w ? desired.x : region_w;
            x = region_x + (region_w - w) * 0.5f;
            break;
        case EHorizontalAlignment::Right:
            w = desired.x < region_w ? desired.x : region_w;
            x = region_x + (region_w - w);
            break;
    }

    float y = region_y;
    float h = region_h;
    switch (v_align)
    {
        case EVerticalAlignment::Fill:
            h = region_h;
            break;
        case EVerticalAlignment::Top:
            h = desired.y < region_h ? desired.y : region_h;
            y = region_y;
            break;
        case EVerticalAlignment::Center:
            h = desired.y < region_h ? desired.y : region_h;
            y = region_y + (region_h - h) * 0.5f;
            break;
        case EVerticalAlignment::Bottom:
            h = desired.y < region_h ? desired.y : region_h;
            y = region_y + (region_h - h);
            break;
    }

    return FGeometry(Vector2(x, y), Vector2(w, h));
}
}  // namespace ZSlate
