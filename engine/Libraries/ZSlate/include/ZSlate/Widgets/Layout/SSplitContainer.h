#pragma once

// SSplitContainer: two panes separated by a draggable grip, sized by a
// fraction of the available space (UE SSplitter with two slots driven by
// SizeCoefficient). Works in both orientations:
//   Vertical   -> panes stacked top / bottom
//   Horizontal -> panes side by side
// Unlike SSplitter (fixed-pixel left pane) the split keeps its proportion
// when the container is resized.

#include "ZSlate/Widgets/SWidget.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>

namespace ZSlate
{

class SSplitContainer : public SWidget
{
public:
    EOrientation Orientation {EOrientation::Vertical};

    // Share of the content area (grip excluded) given to the first pane.
    float SplitFraction     {0.5f};
    float MinPaneSize       {40.0f};
    float SplitterThickness {6.0f};

    UIColor GripColor        {0.22f, 0.24f, 0.29f, 1.0f};
    UIColor GripHoverColor   {0.36f, 0.48f, 0.70f, 1.0f};

    std::function<void(float fraction)> OnSplitChanged;

    SSplitContainer()
    {
        m_Grip = std::make_shared<FGrip>();
        m_Grip->Owner = this;
    }

    // index 0 = first pane (top / left), 1 = second pane (bottom / right).
    void SetSlot(int32_t index, std::shared_ptr<SWidget> child)
    {
        if (index < 0 || index > 1) return;
        m_Panes[static_cast<size_t>(index)] = std::move(child);
    }
    std::shared_ptr<SWidget> GetSlot(int32_t index) const
    {
        return (index < 0 || index > 1) ? nullptr : m_Panes[static_cast<size_t>(index)];
    }

    void SetSplitFraction(float fraction)
    {
        const float total = GetContentLength(GetCachedGeometry());
        float lo = 0.0f;
        float hi = 1.0f;
        if (total > 2.0f * MinPaneSize)
        {
            lo = MinPaneSize / total;
            hi = 1.0f - lo;
        }
        const float clamped = std::clamp(fraction, lo, hi);
        if (std::abs(clamped - SplitFraction) < 0.0001f) return;
        SplitFraction = clamped;
        if (OnSplitChanged) OnSplitChanged(SplitFraction);
    }

    // ---- SWidget -----------------------------------------------------------
    Vector2 ComputeDesiredSize() const override
    {
        const Vector2 a = m_Panes[0] ? m_Panes[0]->GetDesiredSize() : Vector2(0.0f, 0.0f);
        const Vector2 b = m_Panes[1] ? m_Panes[1]->GetDesiredSize() : Vector2(0.0f, 0.0f);
        return IsStacked() ? Vector2(std::max(a.x, b.x), a.y + b.y + SplitterThickness)
                           : Vector2(a.x + b.x + SplitterThickness, std::max(a.y, b.y));
    }

    int GetChildCount() const override { return 3; }
    std::shared_ptr<SWidget> GetChildAt(int index) const override
    {
        switch (index)
        {
        case 0: return m_Panes[0];
        case 1: return m_Grip;
        case 2: return m_Panes[1];
        default: return nullptr;
        }
    }

    void ArrangeChildren(const FGeometry& allotted, std::vector<FArrangedWidget>& out) const override
    {
        const bool  stacked = IsStacked();
        const float total   = GetContentLength(allotted);
        const float first   = std::floor(total * std::clamp(SplitFraction, 0.0f, 1.0f));
        const float second  = std::max(0.0f, total - first);
        const Vector2 size  = allotted.LocalSize;

        if (m_Panes[0] && m_Panes[0]->IsVisible())
            out.push_back({m_Panes[0], allotted.MakeChild(Vector2(0.0f, 0.0f),
                                                          stacked ? Vector2(size.x, first) : Vector2(first, size.y))});

        out.push_back({m_Grip, allotted.MakeChild(stacked ? Vector2(0.0f, first) : Vector2(first, 0.0f),
                                                  stacked ? Vector2(size.x, SplitterThickness)
                                                          : Vector2(SplitterThickness, size.y))});

        const float offset = first + SplitterThickness;
        if (m_Panes[1] && m_Panes[1]->IsVisible())
            out.push_back({m_Panes[1], allotted.MakeChild(stacked ? Vector2(0.0f, offset) : Vector2(offset, 0.0f),
                                                          stacked ? Vector2(size.x, second) : Vector2(second, size.y))});
    }

private:
    // Draggable bar between the two panes.
    struct FGrip : public SWidget
    {
        SSplitContainer* Owner {nullptr};
        bool    Hovered {false};
        bool    Dragging {false};
        float   LastAlong {0.0f};

        Vector2 ComputeDesiredSize() const override { return Vector2(0.0f, 0.0f); }

        void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
        {
            if (!ctx.Renderer || !Owner) return;
            ctx.Renderer->DrawQuad(geom.ToRect(), (Hovered || Dragging) ? Owner->GripHoverColor : Owner->GripColor);
        }

        ECursorType GetCursor() const override
        {
            return (Owner && Owner->IsStacked()) ? ECursorType::ResizeNS : ECursorType::ResizeEW;
        }

        void OnMouseEnter() override { Hovered = true; }
        void OnMouseLeave() override { Hovered = false; }
        void OnMouseCaptureLost() override { Dragging = false; }

        FReply OnMouseButtonDown(const Vector2& pos, int button) override
        {
            if (button != 0 || !Owner) return FReply::Unhandled();
            Dragging  = true;
            LastAlong = Owner->IsStacked() ? pos.y : pos.x;
            return FReply::Handled().CaptureMouse(this);
        }

        void OnMouseMove(const Vector2& pos) override
        {
            if (!Dragging || !Owner) return;
            const float along = Owner->IsStacked() ? pos.y : pos.x;
            const float total = Owner->GetContentLength(Owner->GetCachedGeometry());
            if (total > 0.0f && along != LastAlong)
                Owner->SetSplitFraction(Owner->SplitFraction + (along - LastAlong) / total);
            LastAlong = along;
        }

        FReply OnMouseButtonUp(const Vector2& /*pos*/, int button) override
        {
            if (button != 0 || !Dragging) return FReply::Unhandled();
            Dragging = false;
            return FReply::Handled().ReleaseMouseCapture();
        }
    };

    bool IsStacked() const { return Orientation == EOrientation::Vertical; }

    float GetContentLength(const FGeometry& geom) const
    {
        return std::max(0.0f, (IsStacked() ? geom.LocalSize.y : geom.LocalSize.x) - SplitterThickness);
    }

    std::array<std::shared_ptr<SWidget>, 2> m_Panes;
    std::shared_ptr<FGrip>                  m_Grip;
};

}  // namespace ZSlate
