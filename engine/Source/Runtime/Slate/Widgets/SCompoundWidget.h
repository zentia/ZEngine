#pragma once

#include "Runtime/Slate/Widgets/SWidget.h"

namespace ZSlate
{
// A widget that wraps exactly one content child and adds padding + alignment
// (UE Slate SCompoundWidget analogue). Base for SBox / SBorder / SButton.
class SCompoundWidget : public SWidget
{
public:
    FMargin Padding;
    EHorizontalAlignment HAlign {EHorizontalAlignment::Fill};
    EVerticalAlignment VAlign {EVerticalAlignment::Fill};

    void SetContent(std::shared_ptr<SWidget> content) { m_Content = std::move(content); }
    std::shared_ptr<SWidget> GetContent() const { return m_Content; }

    int GetChildCount() const override { return m_Content ? 1 : 0; }
    std::shared_ptr<SWidget> GetChildAt(int index) const override { return index == 0 ? m_Content : nullptr; }

    Vector2 ComputeDesiredSize() const override
    {
        Vector2 content_size(0.0f, 0.0f);
        if (m_Content && m_Content->Visibility != EVisibility::Collapsed)
            content_size = m_Content->GetDesiredSize();
        return Vector2(content_size.x + Padding.GetTotalHorizontal(),
                       content_size.y + Padding.GetTotalVertical());
    }

    void ArrangeChildren(const FGeometry& allotted, std::vector<FArrangedWidget>& out) const override
    {
        if (!m_Content || m_Content->Visibility == EVisibility::Collapsed)
            return;

        const float region_x = allotted.AbsolutePosition.x + Padding.Left;
        const float region_y = allotted.AbsolutePosition.y + Padding.Top;
        const float region_w = allotted.LocalSize.x - Padding.GetTotalHorizontal();
        const float region_h = allotted.LocalSize.y - Padding.GetTotalVertical();

        const FGeometry child_geom = AlignChildInRegion(
            region_x, region_y, region_w, region_h, m_Content->GetDesiredSize(), HAlign, VAlign);
        out.push_back({m_Content, child_geom});
    }

protected:
    std::shared_ptr<SWidget> m_Content;
};
}  // namespace ZSlate
