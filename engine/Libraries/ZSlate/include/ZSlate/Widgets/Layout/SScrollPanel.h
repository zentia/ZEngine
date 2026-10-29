#pragma once

// ============================================================================
// SScrollPanel — a scrollable content widget with its scrollbar beside it
// ============================================================================
// Two-child layout: the content takes everything left of the bar, the bar takes
// its own desired width on the right.
//
// Why this exists rather than SScrollBox doing it internally: SScrollBox owns a
// scrollbar only through SetScrollBar(), which does not make the bar a child --
// so nothing arranges it and it never gets a geometry. Getting a visible bar
// needs a parent that lays the two out side by side, and every tool that wanted
// one was writing the same two-child ArrangeChildren. This is that panel.
//
// Content is any SWidget, not just SScrollBox: the bar is generic, so a virtual
// list or a hand-painted surface can sit next to it too.

#include "ZSlate/Widgets/Layout/SScrollBox.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace ZSlate
{

class SScrollPanel : public SWidget
{
public:
    SScrollPanel(std::shared_ptr<SWidget> content, std::shared_ptr<SScrollBar> bar)
        : m_Content(std::move(content)), m_Bar(std::move(bar)) {}

    int GetChildCount() const override { return 2; }

    std::shared_ptr<SWidget> GetChildAt(int index) const override
    {
        if (index == 0) return m_Content;
        if (index == 1) return m_Bar;
        return nullptr;
    }

    Vector2 ComputeDesiredSize() const override
    {
        Vector2 size = m_Content ? m_Content->ComputeDesiredSize() : Vector2(0.0f, 0.0f);
        if (m_Bar) size.x += m_Bar->ComputeDesiredSize().x;
        return size;
    }

    void ArrangeChildren(const FGeometry& geometry,
                         std::vector<FArrangedWidget>& children) const override
    {
        const float width  = std::max(0.0f, geometry.LocalSize.x);
        const float height = std::max(0.0f, geometry.LocalSize.y);
        const float bar_width = m_Bar ? std::clamp(m_Bar->ComputeDesiredSize().x, 0.0f, width)
                                      : 0.0f;
        const float content_width = width - bar_width;

        if (m_Content)
            children.push_back({m_Content, geometry.MakeChild(Vector2(0.0f, 0.0f),
                                                              Vector2(content_width, height))});
        if (m_Bar)
            children.push_back({m_Bar, geometry.MakeChild(Vector2(content_width, 0.0f),
                                                          Vector2(bar_width, height))});
    }

private:
    std::shared_ptr<SWidget>   m_Content;
    std::shared_ptr<SScrollBar> m_Bar;
};

}  // namespace ZSlate
