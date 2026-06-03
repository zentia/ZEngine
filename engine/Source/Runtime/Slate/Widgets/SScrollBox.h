#pragma once

#include "Runtime/Slate/Widgets/SWidget.h"

#include <algorithm>
#include <memory>
#include <vector>

namespace ZSlate
{
// A vertically scrolling container (UE Slate SScrollBox analogue). Stacks
// children top-to-bottom at their desired heights, clips to its own rect, and
// scrolls with the mouse wheel. A thin scrollbar is drawn when content overflows.
class SScrollBox : public SWidget
{
public:
    float ScrollSpeed {40.0f};
    float ScrollBarWidth {6.0f};
    UIColor ScrollBarColor {0.40f, 0.40f, 0.46f, 0.85f};

    void AddChild(std::shared_ptr<SWidget> child) { m_Children.push_back(std::move(child)); }
    void ClearChildren() { m_Children.clear(); m_ScrollOffset = 0.0f; }

    // Request the view to pin to the bottom on the next arrange (console tailing).
    void ScrollToBottom() { m_ScrollToEnd = true; }
    // True when the view is already scrolled to (near) the bottom.
    bool IsAtBottom() const
    {
        const float content = ContentHeight();
        const float view = m_CachedGeometry.LocalSize.y;
        const float max_scroll = (content > view) ? (content - view) : 0.0f;
        return m_ScrollOffset >= max_scroll - 2.0f;
    }

    int GetChildCount() const override { return static_cast<int>(m_Children.size()); }
    std::shared_ptr<SWidget> GetChildAt(int index) const override
    {
        if (index < 0 || index >= static_cast<int>(m_Children.size()))
            return nullptr;
        return m_Children[static_cast<size_t>(index)];
    }

    bool ClipsContent() const override { return true; }

    Vector2 ComputeDesiredSize() const override
    {
        // Desired = widest child x total stacked height (a parent Fill slot caps it).
        float width = 0.0f;
        float height = 0.0f;
        for (const std::shared_ptr<SWidget>& child : m_Children)
        {
            if (!child || child->Visibility == EVisibility::Collapsed)
                continue;
            const Vector2 d = child->GetDesiredSize();
            if (d.x > width)
                width = d.x;
            height += d.y;
        }
        return Vector2(width, height);
    }

    void ArrangeChildren(const FGeometry& allotted, std::vector<FArrangedWidget>& out) const override
    {
        if (m_ScrollToEnd)
        {
            const float content = ContentHeight();
            const float view = allotted.LocalSize.y;
            m_ScrollOffset = (content > view) ? (content - view) : 0.0f;
            m_ScrollToEnd = false;
        }

        const float view_w = allotted.LocalSize.x;
        float cursor_y = allotted.AbsolutePosition.y - m_ScrollOffset;
        for (const std::shared_ptr<SWidget>& child : m_Children)
        {
            if (!child || child->Visibility == EVisibility::Collapsed)
                continue;
            const float h = child->GetDesiredSize().y;
            out.push_back({child, FGeometry(Vector2(allotted.AbsolutePosition.x, cursor_y), Vector2(view_w, h))});
            cursor_y += h;
        }
    }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (ctx.Renderer == nullptr)
            return;
        const float content = ContentHeight();
        const float view = geom.LocalSize.y;
        if (content <= view)
            return;

        const UIRect rect = geom.ToRect();
        const float thumb_h = view * (view / content);
        const float max_scroll = content - view;
        const float t = (max_scroll > 0.0f) ? (m_ScrollOffset / max_scroll) : 0.0f;
        const float thumb_y = rect.y + t * (view - thumb_h);
        ctx.Renderer->drawQuad(
            UIRect(rect.x + rect.width - ScrollBarWidth, thumb_y, ScrollBarWidth, thumb_h), ScrollBarColor);
    }

    FReply OnMouseWheel(const Vector2& /*pos*/, float delta) override
    {
        const float content = ContentHeight();
        const float view = m_CachedGeometry.LocalSize.y;
        const float max_scroll = (content > view) ? (content - view) : 0.0f;
        m_ScrollOffset -= delta * ScrollSpeed;
        if (m_ScrollOffset < 0.0f)
            m_ScrollOffset = 0.0f;
        if (m_ScrollOffset > max_scroll)
            m_ScrollOffset = max_scroll;
        return FReply::Handled();
    }

    // Grab the scrollbar thumb (or click the track to jump to it) and drag. Only
    // presses inside the right-edge scrollbar band are consumed, so clicks on the
    // log rows still bubble up normally. Capture keeps the drag tracking even when
    // the cursor leaves the thin bar (matches every other draggable Slate widget).
    FReply OnMouseButtonDown(const Vector2& pos, int button) override
    {
        if (button != 0)
            return FReply::Unhandled();
        const float content = ContentHeight();
        const UIRect rect = m_CachedGeometry.ToRect();
        const float view = rect.height;
        if (content <= view || view <= 0.0f)
            return FReply::Unhandled();  // no overflow -> no bar to grab

        // Generous grab band on the right edge (wider than the drawn bar so it is
        // easy to hit at any DPI).
        const float band = std::max(ScrollBarWidth, 12.0f);
        if (pos.x < rect.x + rect.width - band)
            return FReply::Unhandled();

        const float thumb_h = view * (view / content);
        const float max_scroll = content - view;
        const float t = (max_scroll > 0.0f) ? (m_ScrollOffset / max_scroll) : 0.0f;
        const float thumb_y = rect.y + t * (view - thumb_h);
        if (pos.y >= thumb_y && pos.y <= thumb_y + thumb_h)
        {
            m_DragGrab = pos.y - thumb_y;  // grab the thumb where it was clicked
        }
        else
        {
            m_DragGrab = thumb_h * 0.5f;  // track click: centre the thumb on the cursor
            ApplyThumbDrag(pos.y, rect, content);
        }
        m_DraggingThumb = true;
        return FReply::Handled().CaptureMouse(this);
    }

    void OnMouseMove(const Vector2& pos) override
    {
        if (m_DraggingThumb)
            ApplyThumbDrag(pos.y, m_CachedGeometry.ToRect(), ContentHeight());
    }

    FReply OnMouseButtonUp(const Vector2& /*pos*/, int button) override
    {
        if (button != 0)
            return FReply::Unhandled();
        m_DraggingThumb = false;
        return FReply::Handled().ReleaseMouseCapture();
    }

    void OnMouseCaptureLost() override { m_DraggingThumb = false; }

private:
    float ContentHeight() const
    {
        float height = 0.0f;
        for (const std::shared_ptr<SWidget>& child : m_Children)
        {
            if (child && child->Visibility != EVisibility::Collapsed)
                height += child->GetDesiredSize().y;
        }
        return height;
    }

    // Map a pointer y (screen space) to a scroll offset, keeping the thumb anchored
    // under the grab point. `content`/`rect` are passed in so callers reuse values
    // they already computed this frame.
    void ApplyThumbDrag(float pointer_y, const UIRect& rect, float content)
    {
        const float view = rect.height;
        if (content <= view || view <= 0.0f)
            return;
        const float thumb_h = view * (view / content);
        const float track = view - thumb_h;
        if (track <= 0.0f)
        {
            m_ScrollOffset = 0.0f;
            return;
        }
        float thumb_y = pointer_y - m_DragGrab - rect.y;  // track-relative thumb top
        if (thumb_y < 0.0f)
            thumb_y = 0.0f;
        if (thumb_y > track)
            thumb_y = track;
        m_ScrollOffset = (thumb_y / track) * (content - view);
    }

    std::vector<std::shared_ptr<SWidget>> m_Children;
    mutable float m_ScrollOffset {0.0f};
    mutable bool m_ScrollToEnd {false};
    bool m_DraggingThumb {false};
    float m_DragGrab {0.0f};
};
}  // namespace ZSlate
