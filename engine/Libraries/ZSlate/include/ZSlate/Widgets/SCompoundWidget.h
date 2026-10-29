#pragma once

#include "ZSlate/Core/SlateGeometry.h"
#include "ZSlate/Widgets/SWidget.h"

#include <algorithm>

namespace ZSlate
{

// SCompoundWidget: base class for widgets that own exactly one child
// (UE SCompoundWidget analogue). The child is laid out inside the allotted
// geometry deflated by Padding and aligned with HAlign/VAlign; it is exposed
// through GetChildAt() so it takes part in desired-size caching and
// hit-testing like any other child.
class SCompoundWidget : public SWidget
{
public:
    std::shared_ptr<SWidget> m_Child;

    FMargin   Padding;
    EHorizontalAlignment HAlign {EHorizontalAlignment::Fill};
    EVerticalAlignment VAlign {EVerticalAlignment::Fill};

    void SetContent(std::shared_ptr<SWidget> w) { m_Child = std::move(w); }
    const std::shared_ptr<SWidget>& GetContent() const { return m_Child; }

    int GetChildCount() const override { return m_Child ? 1 : 0; }
    std::shared_ptr<SWidget> GetChildAt(int index) const override { return index == 0 ? m_Child : nullptr; }

    Vector2 ComputeDesiredSize() const override
    {
        Vector2 size(Padding.GetTotalHorizontal(), Padding.GetTotalVertical());
        if (m_Child && m_Child->Visibility != EVisibility::Collapsed)
        {
            const Vector2 child = m_Child->GetDesiredSize();
            size.x += child.x;
            size.y += child.y;
        }
        return size;
    }

    void ArrangeChildren(const FGeometry& allotted, std::vector<FArrangedWidget>& out) const override
    {
        if (!m_Child || m_Child->Visibility == EVisibility::Collapsed)
            return;

        const float x = allotted.AbsolutePosition.x + Padding.Left;
        const float y = allotted.AbsolutePosition.y + Padding.Top;
        const float w = std::max(0.0f, allotted.LocalSize.x - Padding.GetTotalHorizontal());
        const float h = std::max(0.0f, allotted.LocalSize.y - Padding.GetTotalVertical());
        out.push_back(FArrangedWidget{m_Child, AlignChildInRegion(x, y, w, h, m_Child->GetDesiredSize(), HAlign, VAlign)});
    }
};

}  // namespace ZSlate
