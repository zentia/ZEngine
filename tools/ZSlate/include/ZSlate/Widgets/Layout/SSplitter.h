#pragma once

// ============================================================================
// ZSlate/Widgets/Layout/SSplitter.h
//
// Two-pane container with a draggable divider — the WinForms SplitContainer /
// UE SSplitter analogue.
//
//   auto split = std::make_shared<ZSlate::SSplitContainer>();
//   split->Orientation   = ZSlate::EOrientation::Vertical;   // top | bottom
//   split->SetSplitFraction(0.62f);                          // first pane share
//   split->SetSlot(0, topPanel);
//   split->SetSlot(1, bottomPanel);
//
// Orientation::Vertical stacks the panes top/bottom (the grip is a horizontal
// bar); Orientation::Horizontal puts them side by side (vertical bar).
//
// SSplitter split responsibility with SSplitContainer: the grip knows nothing
// about panes, it only reports how far it was dragged; the container turns that
// into a fraction. That keeps either usable on its own.
// ============================================================================

#include "../SWidget.h"
#include "../../Core/SlatePaint.h"
#include "../../Core/ZSlateTypes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

namespace ZSlate
{

// ---------------------------------------------------------------------------
// SSplitter — the draggable grip itself.
// ---------------------------------------------------------------------------
class SSplitter : public SWidget
{
public:
    // Vertical => the grip is a horizontal bar and dragging moves it up/down.
    EOrientation Orientation = EOrientation::Vertical;
    float        Thickness   = 6.0f;

    UIColor Color     { 0.20f, 0.22f, 0.27f, 1.0f };
    UIColor DragColor { 0.35f, 0.55f, 0.95f, 1.0f };
    UIColor GripColor { 0.45f, 0.48f, 0.56f, 1.0f };

    // Movement along the main axis, in pixels.
    std::function<void(float deltaPixels)> OnDragged;

    bool IsDragging() const { return m_Dragging; }

    Vector2 ComputeDesiredSize() const override
    {
        return (Orientation == EOrientation::Vertical)
            ? Vector2(0.0f, Thickness)   // spans the width, Thickness tall
            : Vector2(Thickness, 0.0f);  // spans the height, Thickness wide
    }

    void OnPaint(const FPaintContext& context, const FGeometry& geometry) const override
    {
        if (!context.Renderer) return;

        const UIRect r = geometry.ToRect();
        context.Renderer->DrawQuad(r, m_Dragging ? DragColor : Color);

        // Three grip dots centred on the bar, so it reads as draggable.
        const bool   vertical = (Orientation == EOrientation::Vertical);
        const float  dot      = 2.0f;
        const float  gap      = 3.0f;
        const float  total    = dot * 3.0f + gap * 2.0f;

        for (int i = 0; i < 3; ++i)
        {
            const float offset = -total * 0.5f + static_cast<float>(i) * (dot + gap);
            const UIRect dotRect =
                vertical ? UIRect(r.x + r.w * 0.5f + offset, r.y + r.h * 0.5f - dot * 0.5f, dot, dot)
                         : UIRect(r.x + r.w * 0.5f - dot * 0.5f, r.y + r.h * 0.5f + offset, dot, dot);
            context.Renderer->DrawQuad(dotRect, GripColor);
        }
    }

    FReply OnMouseButtonDown(const Vector2& absolutePos, int button) override
    {
        if (button != 0) return FReply::Unhandled();
        m_Dragging = true;
        m_LastPos  = absolutePos;
        return FReply::Handled();
    }

    FReply OnMouseMove(const Vector2& absolutePos) override
    {
        if (!m_Dragging) return FReply::Unhandled();

        const float delta = (Orientation == EOrientation::Vertical)
            ? absolutePos.y - m_LastPos.y
            : absolutePos.x - m_LastPos.x;
        m_LastPos = absolutePos;

        if (delta != 0.0f && OnDragged) OnDragged(delta);
        return FReply::Handled();
    }

    FReply OnMouseButtonUp(const Vector2&, int button) override
    {
        if (button != 0) return FReply::Unhandled();
        m_Dragging = false;
        return FReply::Handled();
    }

private:
    bool    m_Dragging = false;
    Vector2 m_LastPos  { 0.0f, 0.0f };
};

// ---------------------------------------------------------------------------
// SSplitContainer
// ---------------------------------------------------------------------------
class SSplitContainer : public SWidget
{
public:
    // Vertical => panes stacked top/bottom; Horizontal => left/right.
    EOrientation Orientation = EOrientation::Vertical;

    // Share of the *content* area (grip excluded) given to the first pane.
    float SplitFraction     = 0.5f;
    float MinPaneSize       = 40.0f;
    float SplitterThickness = 6.0f;

    std::function<void(float fraction)> OnSplitChanged;

    SSplitContainer()
    {
        m_Splitter = std::make_shared<SSplitter>();
        m_Splitter->SetParent(this);
        m_Splitter->OnDragged = [this](float deltaPixels) { OnSplitterDragged(deltaPixels); };
    }

    void SetSlot(int32_t index, const std::shared_ptr<SWidget>& child)
    {
        if (index < 0 || index > 1) return;
        if (m_Slots[static_cast<size_t>(index)])
            m_Slots[static_cast<size_t>(index)]->SetParent(nullptr);
        m_Slots[static_cast<size_t>(index)] = child;
        if (child) child->SetParent(this);
    }

    std::shared_ptr<SWidget> GetSlot(int32_t index) const
    {
        if (index < 0 || index > 1) return nullptr;
        return m_Slots[static_cast<size_t>(index)];
    }

    const std::shared_ptr<SSplitter>& GetSplitter() const { return m_Splitter; }

    void SetSplitFraction(float fraction)
    {
        const float total = GetContentSize();
        float min = 0.0f;
        float max = 1.0f;
        if (total > 2.0f * MinPaneSize && total > 0.0f)
        {
            min = MinPaneSize / total;
            max = 1.0f - MinPaneSize / total;
        }

        const float clamped = std::clamp(fraction, min, max);
        if (std::abs(clamped - SplitFraction) < 0.0001f) return;

        SplitFraction = clamped;
        if (OnSplitChanged) OnSplitChanged(SplitFraction);
    }

    bool WantsFillWidth()  const override { return true; }
    bool WantsFillHeight() const override { return true; }

    Vector2 ComputeDesiredSize() const override
    {
        const Vector2 a = m_Slots[0] ? m_Slots[0]->ComputeDesiredSize() : Vector2(0.0f, 0.0f);
        const Vector2 b = m_Slots[1] ? m_Slots[1]->ComputeDesiredSize() : Vector2(0.0f, 0.0f);

        return (Orientation == EOrientation::Vertical)
            ? Vector2(std::max(a.x, b.x), a.y + b.y + SplitterThickness)
            : Vector2(a.x + b.x + SplitterThickness, std::max(a.y, b.y));
    }

    void OnArrangeChildren(const FGeometry& allottedGeometry,
                           std::vector<FArrangedChild>& outChildren) const override
    {
        const UIRect r       = allottedGeometry.ToRect();
        const bool   stacked = (Orientation == EOrientation::Vertical);
        const float  total   = std::max(0.0f, (stacked ? r.h : r.w) - SplitterThickness);

        const float first  = total * SplitFraction;
        const float second = std::max(0.0f, total - first);

        if (m_Slots[0] && m_Slots[0]->IsVisible())
        {
            outChildren.push_back({ m_Slots[0], FGeometry(Vector2(r.x, r.y),
                stacked ? Vector2(r.w, first) : Vector2(first, r.h)) });
        }

        if (m_Splitter)
        {
            // Keep the grip in sync with the container's configuration.
            m_Splitter->Orientation = Orientation;
            m_Splitter->Thickness   = SplitterThickness;

            const Vector2 gripPos  = stacked ? Vector2(r.x, r.y + first)
                                             : Vector2(r.x + first, r.y);
            const Vector2 gripSize = stacked ? Vector2(r.w, SplitterThickness)
                                             : Vector2(SplitterThickness, r.h);
            outChildren.push_back({ m_Splitter, FGeometry(gripPos, gripSize) });
        }

        if (m_Slots[1] && m_Slots[1]->IsVisible())
        {
            const float offset = first + SplitterThickness;
            const Vector2 pos  = stacked ? Vector2(r.x, r.y + offset)
                                         : Vector2(r.x + offset, r.y);
            const Vector2 size = stacked ? Vector2(r.w, second)
                                         : Vector2(second, r.h);
            outChildren.push_back({ m_Slots[1], FGeometry(pos, size) });
        }
    }

private:
    float GetContentSize() const
    {
        const UIRect r = GetCachedGeometry().ToRect();
        return std::max(0.0f, ((Orientation == EOrientation::Vertical) ? r.h : r.w) - SplitterThickness);
    }

    void OnSplitterDragged(float deltaPixels)
    {
        const float total = GetContentSize();
        if (total <= 0.0f) return;
        SetSplitFraction(SplitFraction + deltaPixels / total);
    }

    std::array<std::shared_ptr<SWidget>, 2> m_Slots;
    std::shared_ptr<SSplitter>              m_Splitter;
};

} // namespace ZSlate
