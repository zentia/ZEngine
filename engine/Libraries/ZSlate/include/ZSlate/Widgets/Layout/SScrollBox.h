#pragma once

#include "ZSlate/Widgets/SPanel.h"
#include "ZSlate/Widgets/Layout/SScrollBar.h"
#include "ZSlate/Core/SlateEnums.h"

namespace ZSlate
{

// =============================================================================
// SScrollBox — scrollable container (UE SScrollBox analogue)
// =============================================================================
//
// Holds a single child (or multiple via an inner panel) and scrolls
// it within a clipped viewport. Needs an attached SScrollBar for user
// interaction.
//
// Usage:
//   auto sb = std::make_shared<SScrollBox>();
//   auto bar = std::make_shared<SScrollBar>();
//   sb->SetScrollBar(bar);
//   sb->AddChild(myTallContent);
//
class SScrollBox : public SPanel
{
public:
    EOrientation Orientation {EOrientation::Vertical};

    // Scroll offset in pixels
    mutable float ScrollOffset {0.0f};

    // Scroll speed in pixels per wheel notch (legacy compat)
    float ScrollSpeed {40.0f};

    // The host lays out the external bar; this box owns its scroll binding.
    void SetScrollBar(std::shared_ptr<SScrollBar> bar)
    {
        if (m_ScrollBar) m_ScrollBar->OnUserScrolled = {};
        m_ScrollBar = std::move(bar);
        if (m_ScrollBar)
            m_ScrollBar->OnUserScrolled = [self = weak_from_this()](float value) {
                if (auto widget = self.lock())
                {
                    auto box = std::static_pointer_cast<SScrollBox>(widget);
                    box->ScrollOffset = value * box->m_MaxScroll;
                }
            };
    }
    std::shared_ptr<SScrollBar> GetScrollBar() const { return m_ScrollBar; }

    // Convenience: add the first child (wraps AddSlot)
    void AddChild(std::shared_ptr<SWidget> child)
    {
        if (!child) return;
        AddSlot().Content = std::move(child);
    }

    // --- Scrolling -----------------------------------------------------------

    // Largest ScrollOffset that still shows content; 0 when everything fits.
    // Written by OnArrangeChildren, so it is only meaningful after a layout pass.
    float GetMaxScroll() const { return m_MaxScroll; }

    // True when scrolled to (or pinned at) the end of the content.
    bool IsAtBottom(float tolerance = 1.0f) const { return ScrollOffset >= m_MaxScroll - tolerance; }

    // Pin to the end. The offset is clamped to m_MaxScroll by the next
    // OnArrangeChildren, so a huge value is safe here and survives the content
    // growing between this call and the next arrange (log consoles call this
    // right after appending a row).
    void ScrollToBottom() { ScrollOffset = 1.0e30f; }

    // --- Layout --------------------------------------------------------------

    Vector2 ComputeDesiredSize() const override
    {
        // Desired size is the max child size along the scroll axis,
        // but constrained along the non-scroll axis to the available size.
        Vector2 maxChildSize {0.0f, 0.0f};
        for (const auto& slot : m_Slots)
        {
            if (!slot.Content || !slot.Content->IsVisible()) continue;
            Vector2 s = slot.Content->ComputeDesiredSize();
            maxChildSize.x = std::max(maxChildSize.x, s.x);
            maxChildSize.y = std::max(maxChildSize.y, s.y);
        }
        return maxChildSize;
    }

    void OnArrangeChildren(const FGeometry& AllottedGeometry,
                           std::vector<FArrangedChild>& OutArrangedChildren) const override
    {
        const UIRect viewport = AllottedGeometry.ToRect();
        float totalChildSize = 0.0f;
        float maxCrossSize = 0.0f;

        for (const auto& slot : m_Slots)
        {
            if (!slot.Content || !slot.Content->IsVisible()) continue;
            Vector2 childDesired = slot.Content->ComputeDesiredSize();
            totalChildSize += (Orientation == EOrientation::Vertical) ? childDesired.y : childDesired.x;
            maxCrossSize = std::max(maxCrossSize,
                (Orientation == EOrientation::Vertical) ? childDesired.x : childDesired.y);
        }

        float viewLen = (Orientation == EOrientation::Vertical) ? viewport.h : viewport.w;
        float maxScroll = std::max(0.0f, totalChildSize - viewLen);
        float clampedOffset = std::clamp(ScrollOffset, 0.0f, maxScroll);
        ScrollOffset = clampedOffset;  // write-back
        m_MaxScroll = maxScroll;

        if (m_ScrollBar)
        {
            m_ScrollBar->ViewFraction = (totalChildSize > 0.0f)
                ? std::min(1.0f, viewLen / totalChildSize) : 1.0f;
            m_ScrollBar->Value = (maxScroll > 0.0f) ? clampedOffset / maxScroll : 0.0f;
            m_ScrollBar->TrackClickStep = (maxScroll > 0.0f) ? viewLen / maxScroll : 0.0f;
            m_ScrollBar->WheelStep = (maxScroll > 0.0f) ? ScrollSpeed / maxScroll : 0.0f;
        }

        // Position children
        float cursor = -clampedOffset;
        for (const auto& slot : m_Slots)
        {
            if (!slot.Content || !slot.Content->IsVisible()) continue;

            Vector2 childDesired = slot.Content->ComputeDesiredSize();
            float childAlong = (Orientation == EOrientation::Vertical) ? childDesired.y : childDesired.x;
            float childCross = (Orientation == EOrientation::Vertical) ? childDesired.x : childDesired.y;

            UIRect childRect;
            if (Orientation == EOrientation::Vertical)
                childRect = UIRect(viewport.x, viewport.y + cursor,
                                   std::max(childCross, viewport.w),
                                   childAlong);
            else
                childRect = UIRect(viewport.x + cursor, viewport.y,
                                   childAlong,
                                   std::max(childCross, viewport.h));

            FGeometry childGeom = AllottedGeometry.MakeChild(
                Vector2(childRect.x - viewport.x, childRect.y - viewport.y),
                Vector2(childRect.w, childRect.h));
            OutArrangedChildren.emplace_back(slot.Content, childGeom);
            cursor += childAlong;
        }
    }

    // --- Paint ---------------------------------------------------------------

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (!ctx.Renderer) return;

        // Clip to viewport
        ctx.Renderer->PushClipRect(geom.ToRect());

        // Arrange + paint children
        std::vector<FArrangedChild> arranged;
        const_cast<SScrollBox*>(this)->OnArrangeChildren(geom, arranged);
        for (const auto& child : arranged)
        {
            if (child.Widget)
                child.Widget->Paint(ctx, child.Geometry);
        }

        ctx.Renderer->PopClipRect();
    }

    // --- Input ---------------------------------------------------------------

    FReply OnMouseWheel(const Vector2&, float delta) override
    {
        ScrollOffset -= delta * ScrollSpeed;

        // Clamp
        float totalSize = 0.0f;
        for (const auto& slot : m_Slots)
        {
            if (slot.Content && slot.Content->IsVisible())
            {
                Vector2 s = slot.Content->ComputeDesiredSize();
                totalSize += (Orientation == EOrientation::Vertical) ? s.y : s.x;
            }
        }
        UIRect r = GetCachedGeometry().ToRect();
        float viewLen = (Orientation == EOrientation::Vertical) ? r.h : r.w;
        ScrollOffset = std::clamp(ScrollOffset, 0.0f, std::max(0.0f, totalSize - viewLen));

        // Sync to scroll bar
        if (m_ScrollBar && totalSize > viewLen)
            m_ScrollBar->Value = ScrollOffset / (totalSize - viewLen);

        return FReply::Handled();
    }

private:
    std::shared_ptr<SScrollBar> m_ScrollBar;
    mutable float m_MaxScroll {0.0f};
};

}  // namespace ZSlate
